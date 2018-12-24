// This code is part of the project "Julienne: A Framework for Parallel Graph
// Algorithms using Work-efficient Bucketing", presented at Symposium on
// Parallelism in Algorithms and Architectures, 2017.
// Copyright (c) 2017 Laxman Dhulipala, Guy Blelloch, and Julian Shun
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights (to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// Implementation of a bucketing structure as described in "Julienne: A
// Framework for Parallel Graph Algorithms using Work-efficient Bucketing"
// (SPAA'17)
//
// Note that in practice very few buckets are opened (while vertices may
// start off in a massive number of buckets). To optimize for this situation we
// only explicitly represent "total_buckets" buckets, and filter the remaining
// vertices that have not yet been processed every time the current range runs
// out of buckets. Experimenting with different values of total_buckets (the
// -nb parameter in applications) is necessary to obtain the best performance.
// This also means that the current code could be optimized to run much faster
// in a case where many buckets will be processed; please contact us if you have
// such a use-case.
#pragma once

#include <limits>
#include <tuple>

#include "maybe.h"
#include "vertex_subset.h"

#include "lib/dyn_arr.h"
#include "lib/macros.h"
#include "lib/utilities.h"

#define CACHE_LINE_S 64

typedef uintE bucket_id;
typedef uintE bucket_dest;

enum bucket_order { decreasing, increasing };

struct bucket {
  size_t id;
  size_t num_filtered;
  vertexSubset identifiers;
  bucket(size_t _id, vertexSubset _identifiers)
      : id(_id), identifiers(_identifiers) {}
};

template <class D>
struct buckets {
 public:
  using id_dyn_arr = dyn_arr<uintE>;

  const uintE null_bkt = std::numeric_limits<uintE>::max();

  // Create a bucketing structure.
  //   n : the number of identifiers
  //   d : map from identifier -> bucket
  //   order : the order to iterate over the buckets
  //   total_buckets: the total buckets to materialize
  //
  //   For an identifier i:
  //   d(i) is the bucket currently containing i
  //   d(i) = UINT_E_MAX if i is not in any bucket
  buckets(size_t _n, D _d, bucket_order _order, size_t _total_buckets)
      : n(_n),
        d(_d),
        order(_order),
        open_buckets(_total_buckets - 1),
        total_buckets(_total_buckets),
        cur_bkt(0),
        max_bkt(_total_buckets),
        num_elms(0),
        allocated(true) {
    // Initialize array consisting of the materialized buckets.
    bkts = pbbs::new_array<id_dyn_arr>(total_buckets);

    // Set the current range being processed based on the order.
    if (order == increasing) {
      auto imap = make_sequence<uintE>(n, [&](size_t i) { return d(i); });
      auto min = [](uintE x, uintE y) { return std::min(x, y); };
      size_t min_b = pbbs::reduce(imap, min);
      cur_range = min_b / open_buckets;
    } else if (order == decreasing) {
      auto imap = make_sequence<uintE>(
          n, [&](size_t i) { return (d(i) == null_bkt) ? 0 : d(i); });
      auto max = [](uintE x, uintE y) { return std::max(x, y); };
      size_t max_b = pbbs::reduce(imap, max);
      cur_range = (max_b + open_buckets) / open_buckets;
    } else {
      std::cout << "Unknown order: " << order
                << ". Must be one of {increasing, decreasing}"
                << "\n";
      abort();
    }

    // Update buckets with all (id, bucket) pairs. Identifiers with bkt =
    // null_bkt are ignored by update_buckets.
    auto get_id_and_bkt = [&](uintE i) -> Maybe<std::tuple<uintE, uintE> > {
      uintE bkt = _d(i);
      if (bkt != null_bkt) {
        bkt = to_range(bkt);
      }
      return Maybe<std::tuple<uintE, uintE> >(std::make_tuple(i, bkt));
    };
    update_buckets(get_id_and_bkt, n);
  }

  // Returns the next non-empty bucket from the bucket structure. The return
  // value's bkt_id is null_bkt when no further buckets remain.
  inline bucket next_bucket() {
    while (!curBucketNonEmpty() && num_elms > 0) {
      _next_bucket();
    }
    if (num_elms == 0) {
      size_t bkt_num = null_bkt;  // no buckets remain
      vertexSubset vs(n);
      return bucket(bkt_num, vs);
    }
    return get_cur_bucket();
  }

  // Computes a bucket_dest for an identifier moving from bucket_id prev to
  // bucket_id next.
  inline bucket_dest get_bucket(const bucket_id& prev,
                                const bucket_id& next) const {
    uintE pb = to_range(prev);
    uintE nb = to_range(next);
    if ((nb != null_bkt) &&
        ((prev == null_bkt) || (pb != nb) || (nb == cur_bkt))) {
      return nb;
    }
    return null_bkt;
  }

  void del() {
    if (allocated) {
      for (size_t i = 0; i < total_buckets; i++) {
        bkts[i].del();
      }
      pbbs::free_array(bkts);
      allocated = false;
    }
  }

  // Updates k identifiers in the bucket structure. The i'th identifier and
  // its bucket_dest are given by F(i).
  template <class F>
  inline size_t update_buckets(F f, size_t k) {
    size_t num_blocks = k / 4096;
    int num_threads = nworkers();
    if (k < pbbs::kSequentialForThreshold || num_threads == 1) {
      return update_buckets_seq(f, k);
    }

    size_t ne_before = num_elms;

    size_t block_bits = pbbs::log2_up(num_blocks);
    num_blocks = 1 << block_bits;
    size_t block_size = (k + num_blocks - 1) / num_blocks;

    uintE* hists = pbbs::new_array_no_init<uintE>((num_blocks + 1) *
                                                  total_buckets * CACHE_LINE_S);
    uintE* outs =
        pbbs::new_array_no_init<uintE>((num_blocks + 1) * total_buckets);

    // 1. Compute per-block histograms
    par_for(0, num_blocks, 1, [&] (size_t i) {
      size_t s = i * block_size;
      size_t e = std::min(s + block_size, k);
      uintE* hist = &(hists[i * total_buckets]);

      for (size_t j = 0; j < total_buckets; j++) {
        hist[j] = 0;
      }
      for (size_t j = s; j < e; j++) {
        auto m = f(j);
        bucket_dest b = std::get<1>(m.t);
        if (m.exists && b != null_bkt) {
          hist[b]++;
        }
      }
    });

    // 2. Aggregate histograms into a single histogram.
    auto get = [&](size_t i) {
      size_t col = i % num_blocks;
      size_t row = i / num_blocks;
      return hists[col * total_buckets + row];
    };

    auto in_map = make_sequence<uintE>(num_blocks * total_buckets, get);
    auto out_map = sequence<uintE>(outs, num_blocks * total_buckets);

    size_t sum = pbbs::scan_add(in_map, out_map);
    outs[num_blocks * total_buckets] = sum;

    // 3. Resize buckets based on the summed histogram.
    for (size_t i = 0; i < total_buckets; i++) {
      size_t num_inc = outs[(i + 1) * num_blocks] - outs[i * num_blocks];
      bkts[i].resize(num_inc);
      num_elms += num_inc;
    }

    // 4. Compute the starting offsets for each block.
    par_for(0, total_buckets, 1, [&] (size_t i) {
      size_t start = outs[i * num_blocks];
      for (size_t j = 0; j < num_blocks; j++) {
        hists[(i * num_blocks + j) * CACHE_LINE_S] =
            outs[i * num_blocks + j] - start;
      }
    });

    // 5. Iterate over blocks again. Insert (id, bkt) into bkt[hists[bkt]]
    // and increment hists[bkt].
    par_for(0, num_blocks, 1, [&] (size_t i) {
      size_t s = i * block_size;
      size_t e = std::min(s + block_size, k);
      // our buckets are now spread out, across outs
      for (size_t j = s; j < e; j++) {
        auto m = f(j);
        uintE v = std::get<0>(m.t);
        bucket_dest b = std::get<1>(m.t);
        if (m.exists && b != null_bkt) {
          size_t ind = hists[(b * num_blocks + i) * CACHE_LINE_S];
          bkts[b].insert(v, ind);
          hists[(b * num_blocks + i) * CACHE_LINE_S]++;
        }
      }
    });

    // 6. Finally, update the size of each bucket.
    for (size_t i = 0; i < total_buckets; i++) {
      size_t num_inc = outs[(i + 1) * num_blocks] - outs[i * num_blocks];
      size_t& m = bkts[i].size;
      m += num_inc;
    }

    pbbs::free_array(hists);
    pbbs::free_array(outs);
    return num_elms - ne_before;
  }

 private:
  const bucket_order order;
  id_dyn_arr* bkts;
  size_t cur_bkt;
  size_t max_bkt;
  size_t cur_range;
  D d;
  size_t n;  // total number of identifiers in the system
  size_t num_elms;
  size_t open_buckets;
  size_t total_buckets;
  bool allocated;

  template <class F>
  inline size_t update_buckets_seq(F& f, size_t n) {
    size_t ne_before = num_elms;
    for (size_t i = 0; i < n; i++) {
      auto m = f(i);
      bucket_dest bkt = std::get<1>(m.t);
      if (m.exists && bkt != null_bkt) {
        bkts[bkt].resize(1);
        insert_in_bucket(bkt, std::get<0>(m.t));
        num_elms++;
      }
    }
    return num_elms - ne_before;
  }

  inline void insert_in_bucket(size_t b, intT val) {
    uintE* dst = bkts[b].A;
    intT size = bkts[b].size;
    dst[size] = val;
    bkts[b].size += 1;
  }

  inline bool curBucketNonEmpty() { return bkts[cur_bkt].size > 0; }

  inline void unpack() {
    size_t m = bkts[open_buckets].size;
    auto _d = d;
    auto tmp = sequence<uintE>(m);
    uintE* A = bkts[open_buckets].A;
    par_for(0, m, pbbs::kSequentialForThreshold, [&] (size_t i)
                    { tmp[i] = A[i]; });
    if (order == increasing) {
      cur_range++;  // increment range
    } else {
      cur_range--;
    }
    bkts[open_buckets].size = 0;  // reset size

    auto g = [&](uintE i) -> Maybe<std::tuple<uintE, uintE> > {
      uintE v = tmp[i];
      uintE bkt = to_range(_d(v));
      return Maybe<std::tuple<uintE, uintE> >(std::make_tuple(v, bkt));
    };

    if (m != num_elms) {
      std::cout << "m = " << m << " num_elms = " << num_elms << "\n";
      cur_bkt = 0;
      std::cout << "curBkt = " << get_cur_bucket_num() << "\n";
      std::cout << "mismatch"
                << "\n";
      assert(m == num_elms);  // corrruption in bucket structure.
    }
    update_buckets(g, m);
    num_elms -= m;
  }

  inline void _next_bucket() {
    cur_bkt++;
    if (cur_bkt == open_buckets) {
      unpack();
      cur_bkt = 0;
    }
  }

  // increasing: [cur_range*open_buckets, (cur_range+1)*open_buckets)
  // decreasing: [(cur_range-1)*open_buckets, cur_range*open_buckets)
  inline bucket_id to_range(uintE bkt) const {
    if (order == increasing) {
      if (bkt <
          cur_range *
              open_buckets) {  // this can happen because of the lazy bucketing
        return null_bkt;
      }
      return (bkt < (cur_range + 1) * open_buckets) ? (bkt % open_buckets)
                                                    : open_buckets;
    } else {
      if (bkt >= (cur_range)*open_buckets) {
        return null_bkt;
      }
      return (bkt >= (cur_range - 1) * open_buckets)
                 ? ((open_buckets - (bkt % open_buckets)) - 1)
                 : open_buckets;
    }
  }

  size_t get_cur_bucket_num() const {
    if (order == increasing) {
      return cur_range * open_buckets + cur_bkt;
    } else {
      return (cur_range) * (open_buckets)-cur_bkt - 1;
    }
  }

  inline bucket get_cur_bucket() {
    id_dyn_arr bkt = bkts[cur_bkt];
    size_t size = bkt.size;
    num_elms -= size;
    uintE* out = pbbs::new_array_no_init<uintE>(size);
    size_t cur_bkt_num = get_cur_bucket_num();
    auto _d = d;
    auto p = [&](size_t i) { return _d(i) == cur_bkt_num; };
    size_t m = pbbs::filterf(bkt.A, out, size, p);
    bkts[cur_bkt].size = 0;
    if (m == 0) {
      pbbs::free_array(out);
      return next_bucket();
    }
    vertexSubset vs(n, m, out);
    auto ret = bucket(cur_bkt_num, vs);
    ret.num_filtered = size;
    return ret;
  }
};

template <class D>
inline buckets<D> make_buckets(size_t n, D d, bucket_order order,
                               size_t total_buckets = 128) {
  return buckets<D>(n, d, order, total_buckets);
}
