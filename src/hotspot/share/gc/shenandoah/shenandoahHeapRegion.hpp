/*
 * Copyright (c) 2013, 2019, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "gc/shared/gc_globals.hpp"
#include "gc/shared/spaceDecorator.hpp"
#include "gc/shenandoah/shenandoahAffiliation.hpp"
#include "gc/shenandoah/shenandoahAgeCensus.hpp"
#include "gc/shenandoah/shenandoahAllocRequest.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahPadding.hpp"
#include "utilities/sizes.hpp"

class VMStructs;
class ShenandoahHeapRegionStateConstant;

class ShenandoahHeapRegion {
  friend class VMStructs;
  friend class ShenandoahHeapRegionStateConstant;
private:
  /*
    Region state is described by a state machine. Transitions are guarded by
    heap lock, which allows changing the state of several regions atomically.
    Region states can be logically aggregated in groups.

      "Empty":
      .................................................................
      .                                                               .
      .                                                               .
      .         Uncommitted  <-------  Committed <------------------------\
      .              |                     |                          .   |
      .              \---------v-----------/                          .   |
      .                        |                                      .   |
      .........................|.......................................   |
                               |                                          |
      "Active":                |                                          |
      .........................|.......................................   |
      .                        |                                      .   |
      .      /-----------------^-------------------\                  .   |
      .      |                                     |                  .   |
      .      v                                     v    "Humongous":  .   |
      .   Regular ---\-----\     ..................O................  .   |
      .     |  ^     |     |     .                 |               .  .   |
      .     |  |     |     |     .                 *---------\     .  .   |
      .     v  |     |     |     .                 v         v     .  .   |
      .    Pinned  Cset    |     .  HStart <--> H/Start   H/Cont   .  .   |
      .       ^    / |     |     .  Pinned         v         |     .  .   |
      .       |   /  |     |     .                 *<--------/     .  .   |
      .       |  v   |     |     .                 |               .  .   |
      .  CsetPinned  |     |     ..................O................  .   |
      .              |     |                       |                  .   |
      .              \-----\---v-------------------/                  .   |
      .                        |                                      .   |
      .........................|.......................................   |
                               |                                          |
      "Trash":                 |                                          |
      .........................|.......................................   |
      .                        |                                      .   |
      .                        v                                      .   |
      .                      Trash ---------------------------------------/
      .                                                               .
      .                                                               .
      .................................................................

    Transition from "Empty" to "Active" is first allocation. It can go from {Uncommitted, Committed}
    to {Regular, "Humongous"}. The allocation may happen in Regular regions too, but not in Humongous.

    Transition from "Active" to "Trash" is reclamation. It can go from CSet during the normal cycle,
    and from {Regular, "Humongous"} for immediate reclamation. The existence of Trash state allows
    quick reclamation without actual cleaning up.

    Transition from "Trash" to "Empty" is recycling. It cleans up the regions and corresponding metadata.
    Can be done asynchronously and in bulk.

    Note how internal transitions disallow logic bugs:
      a) No region can go Empty, unless properly reclaimed/recycled;
      b) No region can go Uncommitted, unless reclaimed/recycled first;
      c) Only Regular regions can go to CSet;
      d) Pinned cannot go Trash, thus it could never be reclaimed until unpinned;
      e) Pinned cannot go CSet, thus it never moves;
      f) Humongous cannot be used for regular allocations;
      g) Humongous cannot go CSet, thus it never moves;
      h) Humongous start can go pinned, and thus can be protected from moves (humongous continuations should
         follow associated humongous starts, not pinnable/movable by themselves);
      i) Empty cannot go Trash, avoiding useless work;
      j) ...
   */

  enum RegionState {
    _empty_uncommitted,       // region is empty and has memory uncommitted
    _empty_committed,         // region is empty and has memory committed
    _regular,                 // region is for regular allocations
    _humongous_start,         // region is the humongous start
    _humongous_cont,          // region is the humongous continuation
    _pinned_humongous_start,  // region is both humongous start and pinned
    _cset,                    // region is in collection set
    _pinned,                  // region is pinned
    _pinned_cset,             // region is pinned and in cset (evac failure path)
    _trash,                   // region contains only trash
    _REGION_STATES_NUM        // last
  };

public:
  static const char* region_state_to_string(RegionState s) {
    switch (s) {
      case _empty_uncommitted:       return "Empty Uncommitted";
      case _empty_committed:         return "Empty Committed";
      case _regular:                 return "Regular";
      case _humongous_start:         return "Humongous Start";
      case _humongous_cont:          return "Humongous Continuation";
      case _pinned_humongous_start:  return "Humongous Start, Pinned";
      case _cset:                    return "Collection Set";
      case _pinned:                  return "Pinned";
      case _pinned_cset:             return "Collection Set, Pinned";
      case _trash:                   return "Trash";
      default:
        ShouldNotReachHere();
        return "";
    }
  }

private:
  // This method protects from accidental changes in enum order:
  int region_state_to_ordinal(RegionState s) const {
    switch (s) {
      case _empty_uncommitted:      return 0;
      case _empty_committed:        return 1;
      case _regular:                return 2;
      case _humongous_start:        return 3;
      case _humongous_cont:         return 4;
      case _cset:                   return 5;
      case _pinned:                 return 6;
      case _trash:                  return 7;
      case _pinned_cset:            return 8;
      case _pinned_humongous_start: return 9;
      default:
        ShouldNotReachHere();
        return -1;
    }
  }

  void report_illegal_transition(const char* method);
  void recycle_internal();

public:
  static int region_states_num() {
    return _REGION_STATES_NUM;
  }

  // Allowed transitions from the outside code:
  void make_regular_allocation(ShenandoahAffiliation affiliation);
  void make_affiliated_maybe();
  void make_regular_bypass();
  void make_humongous_start();
  void make_humongous_cont();
  void make_humongous_start_bypass(ShenandoahAffiliation affiliation);
  void make_humongous_cont_bypass(ShenandoahAffiliation affiliation);
  void make_pinned();
  void make_unpinned();
  void make_cset();
  void make_trash();
  void make_trash_immediate();
  void make_empty();
  void make_uncommitted();
  void make_committed_bypass();

  // Primitive state predicates
  bool is_empty_uncommitted()      const { return state() == _empty_uncommitted; }
  bool is_empty_committed()        const { return state() == _empty_committed; }
  bool is_regular()                const { return state() == _regular; }
  bool is_humongous_continuation() const { return state() == _humongous_cont; }
  bool is_regular_pinned()         const { return state() == _pinned; }
  bool is_trash()                  const { return state() == _trash; }

  // Derived state predicates (boolean combinations of individual states)
  bool static is_empty_state(RegionState state) { return state == _empty_committed || state == _empty_uncommitted; }
  bool static is_humongous_start_state(RegionState state) { return state == _humongous_start || state == _pinned_humongous_start; }
  bool is_empty()                  const { return is_empty_state(this->state()); }
  bool is_active()                 const { auto cur_state = state(); return !is_empty_state(cur_state) && cur_state != _trash; }
  bool is_humongous_start()        const { return is_humongous_start_state(state()); }
  bool is_humongous()              const { auto cur_state = state(); return is_humongous_start_state(cur_state) || cur_state == _humongous_cont; }
  bool is_committed()              const { return !is_empty_uncommitted(); }
  bool is_cset()                   const { auto cur_state = state(); return cur_state == _cset || cur_state == _pinned_cset; }
  bool is_pinned()                 const { auto cur_state = state(); return cur_state == _pinned || cur_state == _pinned_cset || cur_state == _pinned_humongous_start; }

  inline bool is_young() const;
  inline bool is_old() const;
  inline bool is_affiliated() const;

  // Macro-properties:
  bool is_alloc_allowed()          const { auto cur_state = state(); return is_empty_state(cur_state) || cur_state == _regular || cur_state == _pinned; }
  bool is_stw_move_allowed()       const { auto cur_state = state(); return cur_state == _regular || cur_state == _cset || (ShenandoahHumongousMoves && cur_state == _humongous_start); }

  RegionState state()              const { return Atomic::load(&_state); }
  int  state_ordinal()             const { return region_state_to_ordinal(state()); }

  void record_pin();
  void record_unpin();
  size_t pin_count() const;

private:
  static size_t RegionCount;
  static size_t RegionSizeBytes;
  static size_t RegionSizeWords;
  static size_t RegionSizeBytesShift;
  static size_t RegionSizeWordsShift;
  static size_t RegionSizeBytesMask;
  static size_t RegionSizeWordsMask;
  static size_t MaxTLABSizeBytes;
  static size_t MaxTLABSizeWords;

  // Never updated fields
  size_t const _index;
  HeapWord* const _bottom;
  HeapWord* const _end;

  // Rarely updated fields
  HeapWord* _new_top;
  double _empty_time;

  HeapWord* _top_before_promoted;

  // Seldom updated fields
  volatile RegionState _state;
  HeapWord* _coalesce_and_fill_boundary; // for old regions not selected as collection set candidates.

  // Frequently updated fields
  HeapWord* _top;

  size_t _tlab_allocs;
  size_t _gclab_allocs;
  size_t _plab_allocs;

  volatile size_t _live_data;
  volatile size_t _critical_pins;

  HeapWord* volatile _update_watermark;

  uint _age;
  CENSUS_NOISE(uint _youth;)   // tracks epochs of retrograde ageing (rejuvenation)

  ShenandoahSharedFlag _recycling; // Used to indicate that the region is being recycled; see try_recycle*().

  bool _needs_bitmap_reset;

public:
  ShenandoahHeapRegion(HeapWord* start, size_t index, bool committed);

  static const size_t MIN_NUM_REGIONS = 10;

  // Return adjusted max heap size
  static size_t setup_sizes(size_t max_heap_size);

  double empty_time() {
    return _empty_time;
  }

  inline static size_t required_regions(size_t bytes) {
    return (bytes + ShenandoahHeapRegion::region_size_bytes() - 1) >> ShenandoahHeapRegion::region_size_bytes_shift();
  }

  inline static bool requires_humongous(size_t words) {
    return words > ShenandoahHeapRegion::RegionSizeWords;
  }

  inline static size_t region_count() {
    return ShenandoahHeapRegion::RegionCount;
  }

  inline static size_t region_size_bytes() {
    return ShenandoahHeapRegion::RegionSizeBytes;
  }

  inline static size_t region_size_words() {
    return ShenandoahHeapRegion::RegionSizeWords;
  }

  inline static size_t region_size_bytes_shift() {
    return ShenandoahHeapRegion::RegionSizeBytesShift;
  }

  inline static size_t region_size_words_shift() {
    return ShenandoahHeapRegion::RegionSizeWordsShift;
  }

  inline static size_t region_size_bytes_mask() {
    return ShenandoahHeapRegion::RegionSizeBytesMask;
  }

  inline static size_t region_size_words_mask() {
    return ShenandoahHeapRegion::RegionSizeWordsMask;
  }

  // Convert to jint with sanity checking
  inline static jint region_size_bytes_jint() {
    assert (ShenandoahHeapRegion::RegionSizeBytes <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahHeapRegion::RegionSizeBytes;
  }

  // Convert to jint with sanity checking
  inline static jint region_size_words_jint() {
    assert (ShenandoahHeapRegion::RegionSizeWords <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahHeapRegion::RegionSizeWords;
  }

  // Convert to jint with sanity checking
  inline static jint region_size_bytes_shift_jint() {
    assert (ShenandoahHeapRegion::RegionSizeBytesShift <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahHeapRegion::RegionSizeBytesShift;
  }

  // Convert to jint with sanity checking
  inline static jint region_size_words_shift_jint() {
    assert (ShenandoahHeapRegion::RegionSizeWordsShift <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahHeapRegion::RegionSizeWordsShift;
  }

  inline static size_t max_tlab_size_bytes() {
    return ShenandoahHeapRegion::MaxTLABSizeBytes;
  }

  inline static size_t max_tlab_size_words() {
    return ShenandoahHeapRegion::MaxTLABSizeWords;
  }

  inline size_t index() const {
    return _index;
  }

  inline void save_top_before_promote();
  inline HeapWord* get_top_before_promote() const { return _top_before_promoted; }
  inline void restore_top_before_promote();
  inline size_t garbage_before_padded_for_promote() const;

  // If next available memory is not aligned on address that is multiple of alignment, fill the empty space
  // so that returned object is aligned on an address that is a multiple of alignment_in_bytes.  Requested
  // size is in words.  It is assumed that this->is_old().  A pad object is allocated, filled, and registered
  // if necessary to assure the new allocation is properly aligned.  Return nullptr if memory is not available.
  inline HeapWord* allocate_aligned(size_t word_size, ShenandoahAllocRequest &req, size_t alignment_in_bytes);

  // Allocation (return nullptr if full)
  inline HeapWord* allocate(size_t word_size, const ShenandoahAllocRequest& req);

  inline void clear_live_data();
  void set_live_data(size_t s);

  // Increase live data for newly allocated region
  inline void increase_live_data_alloc_words(size_t s);

  // Increase live data for region scanned with GC
  inline void increase_live_data_gc_words(size_t s);

  inline bool has_live() const;
  inline size_t get_live_data_bytes() const;
  inline size_t get_live_data_words() const;

  inline size_t garbage() const;

  void print_on(outputStream* st) const;

  void try_recycle_under_lock();

  void try_recycle();

  inline void begin_preemptible_coalesce_and_fill() {
    _coalesce_and_fill_boundary = _bottom;
  }

  inline void end_preemptible_coalesce_and_fill() {
    _coalesce_and_fill_boundary = _end;
  }

  inline void suspend_coalesce_and_fill(HeapWord* next_focus) {
    _coalesce_and_fill_boundary = next_focus;
  }

  inline HeapWord* resume_coalesce_and_fill() {
    return _coalesce_and_fill_boundary;
  }

  // Coalesce contiguous spans of garbage objects by filling header and registering start locations with remembered set.
  // This is used by old-gen GC following concurrent marking to make old-gen HeapRegions parsable. Old regions must be
  // parsable because the mark bitmap is not reliable during the concurrent old mark.
  // Return true iff region is completely coalesced and filled.  Returns false if cancelled before task is complete.
  bool oop_coalesce_and_fill(bool cancellable);

  // Invoke closure on every reference contained within the humongous object that spans this humongous
  // region if the reference is contained within a DIRTY card and the reference is no more than words following
  // start within the humongous object.
  void oop_iterate_humongous_slice_dirty(OopIterateClosure* cl, HeapWord* start, size_t words, bool write_table) const;

  // Invoke closure on every reference contained within the humongous object starting from start and
  // ending at start + words.
  void oop_iterate_humongous_slice_all(OopIterateClosure* cl, HeapWord* start, size_t words) const;

  HeapWord* block_start(const void* p) const;
  size_t block_size(const HeapWord* p) const;
  bool block_is_obj(const HeapWord* p) const { return p < top(); }

  // Find humongous start region that this region belongs to
  ShenandoahHeapRegion* humongous_start_region() const;

  HeapWord* top() const         { return _top;     }
  void set_top(HeapWord* v)     { _top = v;        }

  HeapWord* new_top() const     { return _new_top; }
  void set_new_top(HeapWord* v) { _new_top = v;    }

  HeapWord* bottom() const      { return _bottom;  }
  HeapWord* end() const         { return _end;     }

  size_t capacity() const       { return byte_size(bottom(), end()); }
  size_t used() const           { return byte_size(bottom(), top()); }
  size_t used_before_promote() const { return byte_size(bottom(), get_top_before_promote()); }
  size_t free() const           { return byte_size(top(),    end()); }

  // Does this region contain this address?
  bool contains(HeapWord* p) const {
    return (bottom() <= p) && (p < top());
  }

  inline void adjust_alloc_metadata(ShenandoahAllocRequest::Type type, size_t);
  void reset_alloc_metadata();
  size_t get_shared_allocs() const;
  size_t get_tlab_allocs() const;
  size_t get_gclab_allocs() const;
  size_t get_plab_allocs() const;

  inline HeapWord* get_update_watermark() const;
  inline void set_update_watermark(HeapWord* w);
  inline void set_update_watermark_at_safepoint(HeapWord* w);

  inline ShenandoahAffiliation affiliation() const;
  inline const char* affiliation_name() const;

  void set_affiliation(ShenandoahAffiliation new_affiliation);

  // Region ageing and rejuvenation
  uint age() const { return _age; }
  CENSUS_NOISE(uint youth() const { return _youth; })

  void increment_age() {
    const uint max_age = markWord::max_age;
    assert(_age <= max_age, "Error");
    if (_age++ >= max_age) {
      _age = max_age;   // clamp
    }
  }

  void reset_age() {
    CENSUS_NOISE(_youth += _age;)
    _age = 0;
  }

  CENSUS_NOISE(void clear_youth() { _youth = 0; })

  inline bool need_bitmap_reset() const {
    return _needs_bitmap_reset;
  }

  inline void set_needs_bitmap_reset() {
    _needs_bitmap_reset = true;
  }

  inline void unset_needs_bitmap_reset() {
    _needs_bitmap_reset = false;
  }

private:
  void decrement_humongous_waste() const;
  void do_commit();
  void do_uncommit();

  inline void internal_increase_live_data(size_t s);

  void set_state(RegionState to);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
