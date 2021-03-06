#include "buffer_cache/alt/page.hpp"

#include "arch/runtime/coroutines.hpp"
#include "buffer_cache/alt/page_cache.hpp"
#include "serializer/serializer.hpp"

namespace alt {

// We pick a weird that forces the logic and performance to not spaz out if the
// access time counter overflows.  Performance degradation is "smooth" if
// access_time_counter_ loops around past INITIAL_ACCESS_TIME -- which shouldn't be a
// problem for now, as long as we increment it one value at a time.
static const uint64_t READ_AHEAD_ACCESS_TIME = evicter_t::INITIAL_ACCESS_TIME - 1;


page_t::page_t(block_id_t block_id, page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      ser_buf_size_(0),
      access_time_(page_cache->evicter().next_access_time()),
      snapshot_refcount_(0) {
    page_cache->evicter().add_not_yet_loaded(this);
    coro_t::spawn_now_dangerously(std::bind(&page_t::load_with_block_id,
                                            this,
                                            block_id,
                                            page_cache));
}

page_t::page_t(block_size_t block_size, scoped_malloc_t<ser_buffer_t> buf,
               page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      ser_buf_size_(block_size.ser_value()),
      buf_(std::move(buf)),
      access_time_(page_cache->evicter().next_access_time()),
      snapshot_refcount_(0) {
    rassert(buf_.has());
    page_cache->evicter().add_to_evictable_unbacked(this);
}

page_t::page_t(scoped_malloc_t<ser_buffer_t> buf,
               const counted_t<standard_block_token_t> &block_token,
               page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      ser_buf_size_(block_token->block_size().ser_value()),
      buf_(std::move(buf)),
      block_token_(block_token),
      access_time_(READ_AHEAD_ACCESS_TIME),
      snapshot_refcount_(0) {
    rassert(buf_.has());
    page_cache->evicter().add_to_evictable_disk_backed(this);
}

page_t::page_t(page_t *copyee, page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      ser_buf_size_(0),
      access_time_(page_cache->evicter().next_access_time()),
      snapshot_refcount_(0) {
    page_cache->evicter().add_not_yet_loaded(this);
    coro_t::spawn_now_dangerously(std::bind(&page_t::load_from_copyee,
                                            this,
                                            copyee,
                                            page_cache));
}

page_t::~page_t() {
    if (destroy_ptr_ != NULL) {
        *destroy_ptr_ = true;
    }
}

void page_t::load_from_copyee(page_t *page, page_t *copyee,
                              page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to atomically set
    // destroy_ptr_ and do some other things.
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    auto_drainer_t::lock_t lock(page_cache->drainer_.get());
    page_ptr_t copyee_ptr(copyee, page_cache);

    // Okay, it's safe to block.
    {
        page_acq_t acq;
        acq.init(copyee, page_cache);
        acq.buf_ready_signal()->wait();

        ASSERT_FINITE_CORO_WAITING;
        if (!page_destroyed) {
            // RSP: If somehow there are no snapshotters of copyee now (besides
            // ourself), maybe we could avoid copying this memory.  We need to
            // carefully track snapshotters anyway, once we're comfortable with that,
            // we could do it.

            uint32_t ser_buf_size = copyee->ser_buf_size_;
            rassert(copyee->buf_.has());
            scoped_malloc_t<ser_buffer_t> buf = page_cache->serializer_->malloc();

            memcpy(buf.get(), copyee->buf_.get(), ser_buf_size);

            page->ser_buf_size_ = ser_buf_size;
            page->buf_ = std::move(buf);
            page->destroy_ptr_ = NULL;

            page_cache->evicter().add_now_loaded_size(page->ser_buf_size_);

            page->pulse_waiters_or_make_evictable(page_cache);
        }
    }
}


void page_t::load_with_block_id(page_t *page, block_id_t block_id,
                                page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to set
    // destroy_ptr_ before blocking the coroutine.
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    auto_drainer_t::lock_t lock(page_cache->drainer_.get());

    scoped_malloc_t<ser_buffer_t> buf;
    counted_t<standard_block_token_t> block_token;
    {
        serializer_t *const serializer = page_cache->serializer_;
        buf = serializer->malloc();  // Call malloc() on our home thread because
                                     // we'll destroy it on our home thread and
                                     // tcmalloc likes that.
        on_thread_t th(serializer->home_thread());
        block_token = serializer->index_read(block_id);
        rassert(block_token.has());
        serializer->block_read(block_token,
                               buf.get(),
                               page_cache->reads_io_account_.get());
    }

    ASSERT_FINITE_CORO_WAITING;
    if (page_destroyed) {
        return;
    }

    rassert(!page->block_token_.has());
    rassert(!page->buf_.has());
    rassert(block_token.has());
    page->ser_buf_size_ = block_token->block_size().ser_value();
    page->buf_ = std::move(buf);
    page->block_token_ = std::move(block_token);
    page->destroy_ptr_ = NULL;
    page_cache->evicter().add_now_loaded_size(page->ser_buf_size_);

    page->pulse_waiters_or_make_evictable(page_cache);
}

void page_t::add_snapshotter() {
    // This may not block, because it's called at the beginning of
    // page_t::load_from_copyee.
    ASSERT_NO_CORO_WAITING;
    ++snapshot_refcount_;
}

void page_t::remove_snapshotter(page_cache_t *page_cache) {
    rassert(snapshot_refcount_ > 0);
    --snapshot_refcount_;
    if (snapshot_refcount_ == 0) {
        // Every page_acq_t is bounded by the lifetime of some page_ptr_t: either the
        // one in current_page_acq_t or its current_page_t or the one in
        // load_from_copyee.
        rassert(waiters_.empty());

        page_cache->evicter().remove_page(this);
        delete this;
    }
}

size_t page_t::num_snapshot_references() {
    return snapshot_refcount_;
}

page_t *page_t::make_copy(page_cache_t *page_cache) {
    page_t *ret = new page_t(this, page_cache);
    return ret;
}

void page_t::pulse_waiters_or_make_evictable(page_cache_t *page_cache) {
    rassert(page_cache->evicter().page_is_in_unevictable_bag(this));
    if (waiters_.empty()) {
        page_cache->evicter().move_unevictable_to_evictable(this);
    } else {
        for (page_acq_t *p = waiters_.head(); p != NULL; p = waiters_.next(p)) {
            // The waiter's not already going to have been pulsed.
            p->buf_ready_signal_.pulse();
        }
    }
}

void page_t::add_waiter(page_acq_t *acq) {
    eviction_bag_t *old_bag
        = acq->page_cache()->evicter().correct_eviction_category(this);
    waiters_.push_back(acq);
    acq->page_cache()->evicter().change_to_correct_eviction_bag(old_bag, this);
    if (buf_.has()) {
        acq->buf_ready_signal_.pulse();
    } else if (destroy_ptr_ != NULL) {
        // Do nothing, the page is currently being loaded.
    } else if (block_token_.has()) {
        coro_t::spawn_now_dangerously(std::bind(&page_t::load_using_block_token,
                                                this,
                                                acq->page_cache()));
    } else {
        crash("An unloaded block is not in a loadable state.");
    }
}

// Unevicts page.
void page_t::load_using_block_token(page_t *page, page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to set
    // destroy_ptr_ before blocking the coroutine.
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    auto_drainer_t::lock_t lock(page_cache->drainer_.get());

    counted_t<standard_block_token_t> block_token = page->block_token_;
    rassert(block_token.has());

    scoped_malloc_t<ser_buffer_t> buf;
    {
        serializer_t *const serializer = page_cache->serializer_;
        buf = serializer->malloc();  // Call malloc() on our home thread because
                                     // we'll destroy it on our home thread and
                                     // tcmalloc likes that.
        on_thread_t th(serializer->home_thread());
        serializer->block_read(block_token,
                               buf.get(),
                               page_cache->reads_io_account_.get());
    }

    ASSERT_FINITE_CORO_WAITING;
    if (page_destroyed) {
        return;
    }

    rassert(page->block_token_.get() == block_token.get());
    rassert(!page->buf_.has());
    rassert(page->ser_buf_size_ == block_token->block_size().ser_value());
    block_token.reset();
    page->buf_ = std::move(buf);
    page->destroy_ptr_ = NULL;

    page->pulse_waiters_or_make_evictable(page_cache);
}

uint32_t page_t::get_page_buf_size() {
    rassert(buf_.has());
    rassert(ser_buf_size_ != 0);
    return block_size_t::unsafe_make(ser_buf_size_).value();
}

void *page_t::get_page_buf(page_cache_t *page_cache) {
    rassert(buf_.has());
    access_time_ = page_cache->evicter().next_access_time();
    return buf_->cache_data;
}

void page_t::reset_block_token() {
    // The page is supposed to have its buffer acquired in reset_block_token -- it's
    // the thing modifying the page.  We thus assume that the page is unevictable and
    // resetting block_token_ doesn't change that.
    rassert(!waiters_.empty());
    block_token_.reset();
}


void page_t::remove_waiter(page_acq_t *acq) {
    eviction_bag_t *old_bag
        = acq->page_cache()->evicter().correct_eviction_category(this);
    waiters_.remove(acq);
    acq->page_cache()->evicter().change_to_correct_eviction_bag(old_bag, this);

    // page_acq_t always has a lesser lifetime than some page_ptr_t.
    rassert(snapshot_refcount_ > 0);
}

void page_t::evict_self() {
    // A page_t can only self-evict if it has a block token.
    rassert(waiters_.empty());
    rassert(block_token_.has());
    rassert(buf_.has());
    buf_.reset();
}


page_acq_t::page_acq_t() : page_(NULL), page_cache_(NULL) {
}

void page_acq_t::init(page_t *page, page_cache_t *page_cache) {
    rassert(page_ == NULL);
    rassert(page_cache_ == NULL);
    rassert(!buf_ready_signal_.is_pulsed());
    page_ = page;
    page_cache_ = page_cache;
    page_->add_waiter(this);
}

page_acq_t::~page_acq_t() {
    if (page_ != NULL) {
        rassert(page_cache_ != NULL);
        page_->remove_waiter(this);
    }
}

bool page_acq_t::has() const {
    return page_ != NULL;
}

signal_t *page_acq_t::buf_ready_signal() {
    return &buf_ready_signal_;
}

uint32_t page_acq_t::get_buf_size() {
    buf_ready_signal_.wait();
    return page_->get_page_buf_size();
}

void *page_acq_t::get_buf_write() {
    buf_ready_signal_.wait();
    page_->reset_block_token();
    return page_->get_page_buf(page_cache_);
}

const void *page_acq_t::get_buf_read() {
    buf_ready_signal_.wait();
    return page_->get_page_buf(page_cache_);
}

page_ptr_t::page_ptr_t() : page_(NULL), page_cache_(NULL) {
}

page_ptr_t::~page_ptr_t() {
    reset();
}

page_ptr_t::page_ptr_t(page_ptr_t &&movee)
    : page_(movee.page_), page_cache_(movee.page_cache_) {
    movee.page_ = NULL;
    movee.page_cache_ = NULL;
}

page_ptr_t &page_ptr_t::operator=(page_ptr_t &&movee) {
    page_ptr_t tmp(std::move(movee));
    std::swap(page_, tmp.page_);
    std::swap(page_cache_, tmp.page_cache_);
    return *this;
}

void page_ptr_t::init(page_t *page, page_cache_t *page_cache) {
    rassert(page_ == NULL && page_cache_ == NULL);
    page_ = page;
    page_cache_ = page_cache;
    if (page_ != NULL) {
        page_->add_snapshotter();
    }
}

void page_ptr_t::reset() {
    if (page_ != NULL) {
        page_t *ptr = page_;
        page_cache_t *cache = page_cache_;
        page_ = NULL;
        page_cache_ = NULL;
        ptr->remove_snapshotter(cache);
    }
}

page_t *page_ptr_t::get_page_for_read() const {
    rassert(page_ != NULL);
    return page_;
}

page_t *page_ptr_t::get_page_for_write(page_cache_t *page_cache) {
    rassert(page_ != NULL);
    if (page_->num_snapshot_references() > 1) {
        page_ptr_t tmp(page_->make_copy(page_cache), page_cache);
        *this = std::move(tmp);
    }
    return page_;
}

}  // namespace alt
