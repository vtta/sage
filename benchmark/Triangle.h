#include "lib/sample_sort.h"
#include "ligra.h"

template <template <class W> class vertex, class W>
struct countF {
  using w_vertex = vertex<W>;
  size_t* counts;
  w_vertex* V;
  countF(w_vertex* _V, size_t* _counts) : V(_V), counts(_counts) {}

  inline bool update(uintE s, uintE d) {
    size_t ct = V[s].intersect(&V[d], s, d);
    int w = __cilkrts_get_worker_number();
    counts[w << 4] += ct;
    return 1;
  }

  inline bool updateAtomic(uintE s, uintE d) {
    size_t ct = V[s].intersect(&V[d], s, d);
    int w = __cilkrts_get_worker_number();
    counts[w << 4] += ct;
    return 1;
  }

  inline bool cond(uintE d) { return cond_true(d); }
};

template <class vertex>
uintE* rankNodes(vertex* V, size_t n) {
  uintE* r = newA(uintE, n);
  uintE* o = newA(uintE, n);

  timer t;
  t.start();
  parallel_for(size_t i = 0; i < n; i++) o[i] = i;
  pbbs::sample_sort(o, n, [&](const uintE u, const uintE v) {
    return V[u].getOutDegree() < V[v].getOutDegree();
  });
  parallel_for(size_t i = 0; i < n; i++) r[o[i]] = i;
  t.stop();
  t.reportTotal("Rank time");
  free(o);
  return r;
}

// Directly call edgemap dense-forward.
template <class vertex, class VS, class F>
vertexSubset emdf(graph<vertex> GA, VS& vs, F f, const flags& fl=0) {
  return edgeMapDenseForward<pbbs::empty>(GA, vs, f, fl);
}

template <template <class W> class vertex, class W>
size_t CountDirected(graph<vertex<W>>& DG, size_t* counts,
                     vertexSubset& Frontier) {
  emdf(DG, Frontier, wrap_em_f<W>(countF<vertex, W>(DG.V, counts)), no_output);
  size_t count = seq::plusReduce(counts, 16 * getWorkers());
  return count;
}

template <template <class W> class vertex, class W>
size_t Triangle(graph<vertex<W>>& GA) {
  timer gt;
  gt.start();
  uintT n = GA.n;
  size_t* counts = newA(size_t, 16 * getWorkers());
  for (size_t i = 0; i < 16 * getWorkers(); i++) {
    counts[i] = 0;
  }
  bool* frontier = newA(bool, n);
  { parallel_for(long i = 0; i < n; i++) frontier[i] = 1; }
  vertexSubset Frontier(n, n, frontier);

  // 1. Rank vertices based on degree
  uintE* rank = rankNodes(GA.V, GA.n);

  // 2. Direct edges to point from lower to higher rank vertices.
  // Note that we currently only store out-neighbors for this graph to save
  // memory.
  auto pack_predicate = wrap_f<W>(
      [&](const uintE& u, const uintE& v) { return rank[u] < rank[v]; });
  auto DG = filter_graph<vertex, W>(GA, pack_predicate);
  gt.stop();
  gt.reportTotal("build graph time");

  // 3. Count triangles on the digraph
  timer ct;
  ct.start();
  size_t count = CountDirected(DG, counts, Frontier);
  Frontier.del();
  free(counts);
  size_t tot = 0;
  DG.del();
  ct.stop();
  ct.reportTotal("count time");
  return count;
}
