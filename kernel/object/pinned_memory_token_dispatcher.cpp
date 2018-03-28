// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT


#include <object/pinned_memory_token_dispatcher.h>

#include <assert.h>
#include <err.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <zxcpp/new.h>
#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <object/bus_transaction_initiator_dispatcher.h>
#include <trace.h>

#define LOCAL_TRACE 0

zx_status_t PinnedMemoryTokenDispatcher::Create(fbl::RefPtr<BusTransactionInitiatorDispatcher> bti,
                                                fbl::RefPtr<VmObject> vmo, size_t offset,
                                                size_t size, uint32_t perms,
                                                fbl::RefPtr<Dispatcher>* dispatcher,
                                                zx_rights_t* rights) {
    LTRACE_ENTRY;
    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset) && IS_PAGE_ALIGNED(size));

    bool is_contiguous;
    if (vmo->is_paged()) {
        // Commit the VMO range, in case it's not already committed.
        zx_status_t status = vmo->CommitRange(offset, size, nullptr);
        if (status != ZX_OK) {
            LTRACEF("vmo->CommitRange failed: %d\n", status);
            return status;
        }

        // Pin the memory to make sure it doesn't change from underneath us for the
        // lifetime of the created PMT.
        status = vmo->Pin(offset, size);
        if (status != ZX_OK) {
            LTRACEF("vmo->Pin failed: %d\n", status);
            return status;
        }

        uint64_t expected_addr = 0;
        auto check_contiguous = [](void* context, size_t offset, size_t index, paddr_t pa) {
            auto expected_addr = static_cast<uint64_t*>(context);
            if (index != 0 && pa != *expected_addr) {
                return ZX_ERR_NOT_FOUND;
            }
            *expected_addr = pa + PAGE_SIZE;
            return ZX_OK;
        };
        status = vmo->Lookup(offset, size, 0, check_contiguous, &expected_addr);
        is_contiguous = (status == ZX_OK);
    } else {
        // This is a physical VMO
        is_contiguous = true;
    }

    // Set up a cleanup function to undo the pin if we need to fail this
    // operation.
    auto unpin_vmo = fbl::MakeAutoCall([vmo, offset, size]() {
        if (vmo->is_paged()) {
            vmo->Unpin(offset, size);
        }
    });

    const size_t min_contig = bti->minimum_contiguity();
    DEBUG_ASSERT(fbl::is_pow2(min_contig));

    fbl::AllocChecker ac;
    const size_t num_addrs = ROUNDUP(size, min_contig) / min_contig;
    fbl::Array<dev_vaddr_t> addr_array(new (&ac) dev_vaddr_t[num_addrs], num_addrs);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    auto pmo = fbl::AdoptRef(new (&ac) PinnedMemoryTokenDispatcher(fbl::move(bti), fbl::move(vmo),
                                                                    offset, size, is_contiguous,
                                                                    fbl::move(addr_array)));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    // Now that the pmt object has been created, it is responsible for
    // unpinning.
    unpin_vmo.cancel();

    zx_status_t status = pmo->MapIntoIommu(perms);
    if (status != ZX_OK) {
        LTRACEF("MapIntoIommu failed: %d\n", status);
        return status;
    }

    *dispatcher = fbl::move(pmo);
    *rights = ZX_DEFAULT_PMT_RIGHTS;
    return ZX_OK;
}

// Used during initialization to set up the IOMMU state for this PMT.
//
// We disable thread-safety analysis here, because this is part of the
// initialization routine before other threads have access to this dispatcher.
zx_status_t PinnedMemoryTokenDispatcher::MapIntoIommu(uint32_t perms) TA_NO_THREAD_SAFETY_ANALYSIS {
    const uint64_t bti_id = bti_->bti_id();
    const size_t min_contig = bti_->minimum_contiguity();
    if (is_contiguous_) {
        constexpr dev_vaddr_t kInvalidAddr = 1;

        dev_vaddr_t vaddr_base = kInvalidAddr;
        // Usermode drivers assume that if they requested a contiguous buffer in
        // memory, then the physical addresses will be contiguous.  Return an
        // error if we can't acutally map the address contiguously.
        size_t remaining = size_;
        size_t curr_offset = offset_;
        while (remaining > 0) {
            dev_vaddr_t vaddr;
            size_t mapped_len;
            zx_status_t status = bti_->iommu()->Map(bti_id, vmo_, curr_offset, remaining, perms,
                                                    &vaddr, &mapped_len);
            if (status != ZX_OK) {
                if (vaddr_base != kInvalidAddr) {
                    bti_->iommu()->Unmap(bti_id, vaddr_base, curr_offset - offset_);
                }
                return status;
            }
            if (vaddr_base == kInvalidAddr) {
                vaddr_base = vaddr;
            } else if (vaddr != vaddr_base + curr_offset - offset_) {
                bti_->iommu()->Unmap(bti_id, vaddr_base, curr_offset - offset_);
                bti_->iommu()->Unmap(bti_id, vaddr, mapped_len);
                return ZX_ERR_INTERNAL;
            }

            curr_offset += mapped_len;
            remaining -= mapped_len;
        }

        mapped_addrs_[0] = vaddr_base;

        for (size_t i = 1; i < mapped_addrs_.size(); ++i) {
            mapped_addrs_[i] = mapped_addrs_[i - 1] + min_contig;
        }
        return ZX_OK;
    }

    size_t remaining = size_;
    uint64_t curr_offset = offset_;
    size_t next_addr_idx = 0;
    while (remaining > 0) {
        dev_vaddr_t vaddr;
        size_t mapped_len;
        zx_status_t status = bti_->iommu()->Map(bti_id, vmo_, curr_offset, remaining, perms,
                                                &vaddr, &mapped_len);
        if (status != ZX_OK) {
            zx_status_t err = UnmapFromIommuLocked();
            ASSERT(err == ZX_OK);
            return status;
        }

        // Ensure we don't end up with any non-terminal chunks that are not |min_contig| in
        // length.
        DEBUG_ASSERT(mapped_len % min_contig == 0 || remaining == mapped_len);

        // Break the range up into chunks of length |min_contig|
        size_t mapped_remaining = mapped_len;
        while (mapped_remaining > 0) {
            size_t addr_pages = fbl::min<size_t>(mapped_remaining, min_contig);
            mapped_addrs_[next_addr_idx] = vaddr;
            next_addr_idx++;
            vaddr += addr_pages;
            mapped_remaining -= addr_pages;
        }

        curr_offset += mapped_len;
        remaining -= fbl::min(mapped_len, remaining);
    }
    DEBUG_ASSERT(next_addr_idx == mapped_addrs_.size());

    return ZX_OK;
}

zx_status_t PinnedMemoryTokenDispatcher::UnmapFromIommuLocked() {
    auto iommu = bti_->iommu();
    const uint64_t bus_txn_id = bti_->bti_id();

    if (mapped_addrs_[0] == UINT64_MAX) {
        // No work to do, nothing is mapped.
        return ZX_OK;
    }

    zx_status_t status = ZX_OK;
    if (is_contiguous_) {
        status = iommu->Unmap(bus_txn_id, mapped_addrs_[0], size_);
    } else {
        const size_t min_contig = bti_->minimum_contiguity();
        size_t remaining = size_;
        for (size_t i = 0; i < mapped_addrs_.size(); ++i) {
            dev_vaddr_t addr = mapped_addrs_[i];
            if (addr == UINT64_MAX) {
                break;
            }

            size_t size = fbl::min(remaining, min_contig);
            DEBUG_ASSERT(size == min_contig || i == mapped_addrs_.size() - 1);
            // Try to unmap all pages even if we get an error, and return the
            // first error encountered.
            zx_status_t err = iommu->Unmap(bus_txn_id, addr, size);
            DEBUG_ASSERT(err == ZX_OK);
            if (err != ZX_OK && status == ZX_OK) {
                status = err;
            }
            remaining -= size;
        }
    }

    // Clear this so we won't try again if this gets called again in the
    // destructor.
    InvalidateMappedAddrsLocked();
    return status;
}

void PinnedMemoryTokenDispatcher::MarkUnpinned() {
    fbl::AutoLock guard(get_lock());
    explicitly_unpinned_ = true;
}

void PinnedMemoryTokenDispatcher::InvalidateMappedAddrsLocked() {
    // Fill with a known invalid address to simplify cleanup of errors during
    // mapping
    for (size_t i = 0; i < mapped_addrs_.size(); ++i) {
        mapped_addrs_[i] = UINT64_MAX;
    }
}

void PinnedMemoryTokenDispatcher::on_zero_handles() {
    fbl::AutoLock guard(get_lock());

    // Once usermode has dropped the handle, either through zx_handle_close(),
    // zx_pmt_unpin(), or process crash, prevent access to the pinned memory.
    //
    // We do not unpin the VMO until this object is destroyed, to allow usermode
    // to protect against stray DMA via the quarantining mechanism.
    zx_status_t status = UnmapFromIommuLocked();
    ASSERT(status == ZX_OK);

    if (explicitly_unpinned_) {
        // The cleanup will happen when the reference that on_zero_handles()
        // was called on goes away.
    } else {
        // Add to the quarantine list to prevent the underlying VMO from being
        // unpinned.
        bti_->Quarantine(fbl::WrapRefPtr(this));
    }
}

PinnedMemoryTokenDispatcher::~PinnedMemoryTokenDispatcher() {
    // In most cases the Unmap will already have run via on_zero_handles(), but
    // it is possible for that to never run if an error occurs between the
    // creation of the PinnedMemoryTokenDispatcher and the completion of the
    // zx_bti_pin() syscall.
    zx_status_t status = UnmapFromIommuLocked();
    ASSERT(status == ZX_OK);

    if (vmo_->is_paged()) {
        vmo_->Unpin(offset_, size_);
    }

    bti_->RemovePmo(this);
}

PinnedMemoryTokenDispatcher::PinnedMemoryTokenDispatcher(
        fbl::RefPtr<BusTransactionInitiatorDispatcher> bti,
        fbl::RefPtr<VmObject> vmo, size_t offset, size_t size,
        bool is_contiguous,
        fbl::Array<dev_vaddr_t> mapped_addrs)
    : vmo_(fbl::move(vmo)), offset_(offset), size_(size), is_contiguous_(is_contiguous),
      bti_(fbl::move(bti)), mapped_addrs_(fbl::move(mapped_addrs)) {

    InvalidateMappedAddrsLocked();
    bti_->AddPmoLocked(this);
}

zx_status_t PinnedMemoryTokenDispatcher::EncodeAddrs(bool compress_results,
                                                     dev_vaddr_t* mapped_addrs,
                                                     size_t mapped_addrs_count) {
    fbl::AutoLock guard(get_lock());

    const fbl::Array<dev_vaddr_t>& pmo_addrs = mapped_addrs_;
    const size_t found_addrs = pmo_addrs.size();
    if (compress_results) {
        if (found_addrs != mapped_addrs_count) {
            return ZX_ERR_INVALID_ARGS;
        }
        memcpy(mapped_addrs, pmo_addrs.get(), found_addrs * sizeof(dev_vaddr_t));
    } else {
        const size_t num_pages = size_ / PAGE_SIZE;
        if (num_pages != mapped_addrs_count) {
            return ZX_ERR_INVALID_ARGS;
        }
        const size_t min_contig = bti_->minimum_contiguity();
        size_t next_idx = 0;
        for (size_t i = 0; i < found_addrs; ++i) {
            dev_vaddr_t extent_base = pmo_addrs[i];
            for (dev_vaddr_t addr = extent_base;
                 addr < extent_base + min_contig && next_idx < num_pages;
                 addr += PAGE_SIZE, ++next_idx) {
                mapped_addrs[next_idx] = addr;
            }
        }
    }
    return ZX_OK;
}