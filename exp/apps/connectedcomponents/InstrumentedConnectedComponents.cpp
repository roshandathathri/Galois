/** Connected components -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Compute the connect components of a graph and optionally write out the largest
 * component to file.
 *
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "Galois/Galois.h"
#include "Galois/Accumulator.h"
#include "Galois/Bag.h"
#include "Galois/DomainSpecificExecutors.h"
#include "Galois/Statistic.h"
#include "Galois/UnionFind.h"
#include "Galois/Graphs/LCGraph.h"
#include "Galois/Graphs/OCGraph.h"
#include "Galois/Graphs/TypeTraits.h"
#include "Galois/ParallelSTL.h"
#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"

#include <utility>
#include <vector>
#include <algorithm>
#include <iostream>

#ifdef GALOIS_USE_EXP
#include "LigraAlgo.h"
#include "GraphLabAlgo.h"
#include "GraphChiAlgo.h"
#endif

#include <ostream>
#include <fstream>

const char* name = "Connected Components";
const char* desc = "Computes the connected components of a graph";
const char* url = 0;

enum Algo {
  async,
  asyncOc,
  blockedasync,
  graphchi,
  graphlab,
  labelProp,
  staleLp, 
  pullLp, 
  pullLpCas,
  ligra,
  ligraChi,
  serial,
  synchronous
};

enum OutputEdgeType {
  void_,
  int32_,
  int64_
};

namespace cll = llvm::cl;
static cll::opt<std::string> inputFilename(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<std::string> largestComponentFilename("outputLargestComponent", cll::desc("[output graph file]"), cll::init(""));
static cll::opt<std::string> permutationFilename("outputNodePermutation", cll::desc("[output node permutation file]"), cll::init(""));
static cll::opt<std::string> transposeGraphName("graphTranspose", cll::desc("Transpose of input graph"));
static cll::opt<bool> symmetricGraph("symmetricGraph", cll::desc("Input graph is symmetric"), cll::init(false));
cll::opt<unsigned int> memoryLimit("memoryLimit",
    cll::desc("Memory limit for out-of-core algorithms (in MB)"), cll::init(~0U));
static cll::opt<OutputEdgeType> writeEdgeType("edgeType", cll::desc("Input/Output edge type:"),
    cll::values(
      clEnumValN(OutputEdgeType::void_, "void", "no edge values"),
      clEnumValN(OutputEdgeType::int32_, "int32", "32 bit edge values"),
      clEnumValN(OutputEdgeType::int64_, "int64", "64 bit edge values"),
      clEnumValEnd), cll::init(OutputEdgeType::void_));
static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumValN(Algo::async, "async", "Asynchronous (default)"),
      clEnumValN(Algo::blockedasync, "blockedasync", "Blocked asynchronous"),
      clEnumValN(Algo::asyncOc, "asyncOc", "Asynchronous out-of-core memory"),
      clEnumValN(Algo::labelProp, "labelProp", "Using label propagation algorithm"),
      clEnumValN(Algo::staleLp, "staleLp", "Using label propagation algorithm w/ possibly stale lables"),
      clEnumValN(Algo::pullLp, "pullLp", "Using pull-based label propagation algorithm"),
      clEnumValN(Algo::pullLpCas, "pullLpCas", "Using pull-based label propagation algorithm w/ CAS"),
      clEnumValN(Algo::serial, "serial", "Serial"),
      clEnumValN(Algo::synchronous, "sync", "Synchronous"),
#ifdef GALOIS_USE_EXP
      clEnumValN(Algo::graphchi, "graphchi", "Using GraphChi programming model"),
      clEnumValN(Algo::graphlab, "graphlab", "Using GraphLab programming model"),
      clEnumValN(Algo::ligraChi, "ligraChi", "Using Ligra and GraphChi programming model"),
      clEnumValN(Algo::ligra, "ligra", "Using Ligra programming model"),
#endif
      clEnumValEnd), cll::init(Algo::async));

static const bool traceWork = true;
static Galois::Statistic* GoodWork;
static Galois::Statistic* EmptyWork;

struct Node: public Galois::UnionFindNode<Node> {
  typedef Node* component_type;
  unsigned int id;

  Node(): Galois::UnionFindNode<Node>(const_cast<Node*>(this)) { }
  Node(const Node& o): Galois::UnionFindNode<Node>(o.m_component), id(o.id) { }

  Node& operator=(const Node& o) {
    Node c(o);
    std::swap(c, *this);
    return *this;
  }

  component_type component() { return this->findAndCompress(); }
};

template<typename Graph>
void readInOutGraph(Graph& graph) {
  using namespace Galois::Graph;
  if (symmetricGraph) {
    Galois::Graph::readGraph(graph, inputFilename);
  } else if (transposeGraphName.size()) {
    Galois::Graph::readGraph(graph, inputFilename, transposeGraphName);
  } else {
    GALOIS_DIE("Graph type not supported");
  }
}

/** 
 * Serial connected components algorithm. Just use union-find.
 */
struct SerialAlgo {
  typedef Galois::Graph::LC_CSR_Graph<Node,void>
    ::with_no_lockable<true>::type Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) { Galois::Graph::readGraph(graph, inputFilename); }

  struct Merge {
    Graph& graph;
    Merge(Graph& g): graph(g) { }

    void operator()(const GNode& src) const {
      Node& sdata = graph.getData(src, Galois::MethodFlag::UNPROTECTED);
      
      for (Graph::edge_iterator ii = graph.edge_begin(src, Galois::MethodFlag::UNPROTECTED),
          ei = graph.edge_end(src, Galois::MethodFlag::UNPROTECTED); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        Node& ddata = graph.getData(dst, Galois::MethodFlag::UNPROTECTED);
        sdata.merge(&ddata);
      }
    }
  };

  void operator()(Graph& graph) {
    std::for_each(graph.begin(), graph.end(), Merge(graph));
  }
};

/**
 * Synchronous connected components algorithm.  Initially all nodes are in
 * their own component. Then, we merge endpoints of edges to form the spanning
 * tree. Merging is done in two phases to simplify concurrent updates: (1)
 * find components and (2) union components.  Since the merge phase does not
 * do any finds, we only process a fraction of edges at a time; otherwise,
 * the union phase may unnecessarily merge two endpoints in the same
 * component.
 */
struct SynchronousAlgo {
  typedef Galois::Graph::LC_CSR_Graph<Node,void>
    ::with_no_lockable<true>::type
    ::with_numa_alloc<true>::type Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) { Galois::Graph::readGraph(graph, inputFilename); }

  struct Edge {
    GNode src;
    Node* ddata;
    int count;
    Edge(GNode src, Node* ddata, int count): src(src), ddata(ddata), count(count) { }
  };

  Galois::InsertBag<Edge> wls[2];
  Galois::InsertBag<Edge>* next;
  Galois::InsertBag<Edge>* cur;

  struct Initialize {
    Graph& graph;
    Galois::InsertBag<Edge>& next;
    Initialize(Graph& g, Galois::InsertBag<Edge>& next): graph(g), next(next) { }

    //! Add the first edge between components to the worklist
    void operator()(const GNode& src) const {
      for (Graph::edge_iterator ii = graph.edge_begin(src, Galois::MethodFlag::UNPROTECTED),
          ei = graph.edge_end(src, Galois::MethodFlag::UNPROTECTED); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        if (symmetricGraph && src >= dst)
          continue;
        Node& ddata = graph.getData(dst, Galois::MethodFlag::UNPROTECTED);
        next.push(Edge(src, &ddata, 0));
        break;
      }
    }
  };

  struct Merge {
    Graph& graph;
    Galois::Statistic& emptyMerges;
    Merge(Graph& g, Galois::Statistic& e): graph(g), emptyMerges(e) { }

    void operator()(const Edge& edge) const {
      Node& sdata = graph.getData(edge.src, Galois::MethodFlag::UNPROTECTED);
      if (!sdata.merge(edge.ddata))
        emptyMerges += 1;
    }
  };

  struct Find {
    typedef int tt_does_not_need_aborts;
    typedef int tt_does_not_need_push;
    typedef int tt_does_not_need_stats;

    Graph& graph;
    Galois::InsertBag<Edge>& next;
    Find(Graph& g, Galois::InsertBag<Edge>& next): graph(g), next(next) { }

    //! Add the next edge between components to the worklist
    void operator()(const Edge& edge, Galois::UserContext<Edge>&) const {
      (*this)(edge);
    }

    void operator()(const Edge& edge) const {
      GNode src = edge.src;
      Node& sdata = graph.getData(src, Galois::MethodFlag::UNPROTECTED);
      Node* scomponent = sdata.findAndCompress();
      Graph::edge_iterator ii = graph.edge_begin(src, Galois::MethodFlag::UNPROTECTED);
      Graph::edge_iterator ei = graph.edge_end(src, Galois::MethodFlag::UNPROTECTED);
      int count = edge.count + 1;
      std::advance(ii, count);
      for (; ii != ei; ++ii, ++count) {
        GNode dst = graph.getEdgeDst(ii);
        if (symmetricGraph && src >= dst)
          continue;
        Node& ddata = graph.getData(dst, Galois::MethodFlag::UNPROTECTED);
        Node* dcomponent = ddata.findAndCompress();
        if (scomponent != dcomponent) {
          next.push(Edge(src, dcomponent, count));
          break;
        }
      }
    }
  };

  void operator()(Graph& graph) {
    Galois::Statistic rounds("Rounds");
    Galois::Statistic emptyMerges("EmptyMerges");

    cur = &wls[0];
    next = &wls[1];
    Galois::do_all_local(graph, Initialize(graph, *cur));

    while (!cur->empty()) {
      Galois::do_all_local(*cur, Merge(graph, emptyMerges));
      Galois::for_each_local(*cur, Find(graph, *next));
      cur->clear();
      std::swap(cur, next);
      rounds += 1;
    }
  }
};

template<bool UseStaleValue>
struct LabelPropAlgo {
  struct LNode {
    typedef unsigned int component_type;
    unsigned int id;
    unsigned int comp;
    
    component_type component() { return comp; }
    bool isRep() { return id == comp; }
  };

  typedef typename Galois::Graph::LC_CSR_Graph<LNode,void>
    ::template with_no_lockable<true>::type
    ::template with_numa_alloc<true>::type InnerGraph;
  typedef Galois::Graph::LC_InOut_Graph<InnerGraph> Graph;
  typedef typename Graph::GraphNode GNode;
  typedef typename LNode::component_type component_type;

  template<typename G>
  void readGraph(G& graph) {
    readInOutGraph(graph);
  }

  struct Initialize {
    Graph& graph;

    Initialize(Graph& g): graph(g) { }
    void operator()(GNode n) const {
      LNode& data = graph.getData(n, Galois::MethodFlag::UNPROTECTED);
      data.comp = data.id;
    }
  };

  template<bool Forward,bool Backward>
  struct Process {
    typedef int tt_does_not_need_aborts;
    Graph& graph;
    Process(Graph& g): graph(g) { }

    template<typename Iterator,typename GetNeighbor>
    void update(LNode& sdata, Iterator ii, Iterator ei, GetNeighbor get, Galois::UserContext<GNode>& ctx) {
      component_type newV;
      if(UseStaleValue) {
        newV = sdata.comp;
      }

      for (; ii != ei; ++ii) {
        GNode dst = get(ii);
        LNode& ddata = graph.getData(dst, Galois::MethodFlag::UNPROTECTED);

        while (true) {
          component_type old = ddata.comp;
          if(!UseStaleValue) {
            newV = sdata.comp;
          }
          if (old <= newV) {
            if(traceWork) {
              *EmptyWork += 1;
            }
            break;
          }
          if (__sync_bool_compare_and_swap(&ddata.comp, old, newV)) {
            if(traceWork) {
              *GoodWork += 1;
            }
            ctx.push(dst);
            break;
          }
        }
      }
    }

    struct BackwardUpdate {
      Graph& graph;
      BackwardUpdate(Graph& g): graph(g) { }
      GNode operator()(typename Graph::in_edge_iterator ii) { return graph.getInEdgeDst(ii); }
    };

    struct ForwardUpdate {
      Graph& graph;
      ForwardUpdate(Graph& g): graph(g) { }
      GNode operator()(typename Graph::edge_iterator ii) { return graph.getEdgeDst(ii); }
    };

    //! Add the next edge between components to the worklist
    void operator()(const GNode& src, Galois::UserContext<GNode>& ctx) {
      LNode& sdata = graph.getData(src, Galois::MethodFlag::UNPROTECTED);
      
      if (Backward) {
        update(sdata, graph.in_edge_begin(src, Galois::MethodFlag::UNPROTECTED), graph.in_edge_end(src, Galois::MethodFlag::UNPROTECTED),
            BackwardUpdate(graph), ctx);
      } 
      if (Forward) {
        update(sdata, graph.edge_begin(src, Galois::MethodFlag::UNPROTECTED), graph.edge_end(src, Galois::MethodFlag::UNPROTECTED),
            ForwardUpdate(graph), ctx);
      }
    }
  };

  void operator()(Graph& graph) {
    typedef Galois::WorkList::dChunkedFIFO<256> WL;

    Galois::do_all_local(graph, Initialize(graph));
    if (symmetricGraph) {
      Galois::for_each_local(graph, Process<true,false>(graph), Galois::wl<WL>());
    } else {
      Galois::for_each_local(graph, Process<true,true>(graph), Galois::wl<WL>());
    }
  }
};

//! Assumes symmetric graph
struct PullLPAlgo {
  struct LNode {
    typedef unsigned int component_type;
    unsigned int id;
    unsigned int comp;
    
    component_type component() { return comp; }
    bool isRep() { return id == comp; }
  };

  typedef Galois::Graph::LC_CSR_Graph<LNode,void>
    ::with_numa_alloc<true>::type InnerGraph;
  typedef Galois::Graph::LC_InOut_Graph<InnerGraph> Graph;
  typedef Graph::GraphNode GNode;
  typedef LNode::component_type component_type;

  template<typename G>
  void readGraph(G& graph) {
    using namespace Galois::Graph;
    if (symmetricGraph) {
      Galois::Graph::readGraph(graph, inputFilename);
    } else {
      GALOIS_DIE("Graph type not supported");
    }
  }

  struct Initialize {
    Graph& graph;

    Initialize(Graph& g): graph(g) { }
    void operator()(GNode n) const {
      LNode& data = graph.getData(n, Galois::MethodFlag::UNPROTECTED);
      data.comp = data.id;
    }
  };

  struct Process {
    Graph& graph;
    Process(Graph& g): graph(g) { }

    //! Add the next edge between components to the worklist
    void operator()(const GNode& n, Galois::UserContext<GNode>& ctx) {
      LNode& data = graph.getData(n);
      component_type old = data.comp, newV = data.comp;
      unsigned int good = 0, empty = 0;
     
      //! Pull from incoming neighbors to update my own label 
      auto inIt = graph.in_edge_begin(n), inIe = graph.in_edge_end(n);
      for( ; inIt != inIe; ++inIt) {
        GNode src = graph.getInEdgeDst(inIt);
        LNode& sData = graph.getData(src);

        if(sData.comp < newV) {
          newV = sData.comp;
          if(traceWork) {
            good += 1;
          }
        } else {
          if(traceWork) {
            empty += 1;
          }
        }
      }

      if(old != newV) {
        data.comp = newV;
        if(traceWork) {
          *GoodWork += good;
        }

        //! Ask my outgoing neighbors to update themselves
        auto outIt = graph.edge_begin(n), outIe = graph.edge_end(n);
        for( ; outIt != outIe; ++outIt) {
          GNode dst = graph.getEdgeDst(outIt);
          ctx.push(dst);
        }
      }

      if(traceWork) {
        *EmptyWork += empty;
      }
    }
  };

  void operator()(Graph& graph) {
    typedef Galois::WorkList::dChunkedFIFO<256> WL;

    Galois::do_all_local(graph, Initialize(graph));
    Galois::for_each_local(graph, Process(graph), Galois::wl<WL>());
  }
};

//! Assumes symmetric graph
struct PullLPCASAlgo {
  struct LNode {
    typedef unsigned int component_type;
    unsigned int id;
    unsigned int comp;
    
    component_type component() { return comp; }
    bool isRep() { return id == comp; }
  };

  typedef Galois::Graph::LC_CSR_Graph<LNode,void>
    ::with_no_lockable<true>::type
    ::with_numa_alloc<true>::type InnerGraph;
  typedef Galois::Graph::LC_InOut_Graph<InnerGraph> Graph;
  typedef Graph::GraphNode GNode;
  typedef LNode::component_type component_type;

  template<typename G>
  void readGraph(G& graph) {
    using namespace Galois::Graph;
    if (symmetricGraph) {
      Galois::Graph::readGraph(graph, inputFilename);
    } else {
      GALOIS_DIE("Graph type not supported");
    }
  }

  struct Initialize {
    Graph& graph;

    Initialize(Graph& g): graph(g) { }
    void operator()(GNode n) const {
      LNode& data = graph.getData(n, Galois::MethodFlag::UNPROTECTED);
      data.comp = data.id;
    }
  };

  struct Process {
    typedef int tt_does_not_need_aborts;
    Graph& graph;
    Process(Graph& g): graph(g) { }

    //! Add the next edge between components to the worklist
    void operator()(const GNode& n, Galois::UserContext<GNode>& ctx) {
      LNode& data = graph.getData(n, Galois::MethodFlag::UNPROTECTED);
      component_type old = data.comp, newV = data.comp;
      unsigned int good = 0, empty = 0;
     
      //! Pull from incoming neighbors to update my own label 
      auto inIt = graph.in_edge_begin(n, Galois::MethodFlag::UNPROTECTED), inIe = graph.in_edge_end(n, Galois::MethodFlag::UNPROTECTED);
      for( ; inIt != inIe; ++inIt) {
        GNode src = graph.getInEdgeDst(inIt);
        LNode& sData = graph.getData(src, Galois::MethodFlag::UNPROTECTED);

        if(sData.comp < newV) {
          newV = sData.comp;
          if(traceWork) {
            *GoodWork += 1;
          }
        } else {
          if(traceWork) {
            *EmptyWork += 1;
          }
        }
      }

      if(old != newV) {
        __sync_bool_compare_and_swap(&(data.comp), old, newV);

        //! Ask my outgoing neighbors to update themselves
        auto outIt = graph.edge_begin(n, Galois::MethodFlag::UNPROTECTED), outIe = graph.edge_end(n, Galois::MethodFlag::UNPROTECTED);
        for( ; outIt != outIe; ++outIt) {
          GNode dst = graph.getEdgeDst(outIt);
          ctx.push(dst);
        }
      }
    }
  };

  void operator()(Graph& graph) {
    typedef Galois::WorkList::dChunkedFIFO<256> WL;

    Galois::do_all_local(graph, Initialize(graph));
    Galois::for_each_local(graph, Process(graph), Galois::wl<WL>());
  }
};

struct AsyncOCAlgo {
  typedef Galois::Graph::OCImmutableEdgeGraph<Node,void> Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) {
    readInOutGraph(graph);
  }

  struct Merge {
    typedef int tt_does_not_need_aborts;
    typedef int tt_does_not_need_push;

    Galois::Statistic& emptyMerges;
    Merge(Galois::Statistic& e): emptyMerges(e) { }

    //! Add the next edge between components to the worklist
    template<typename GTy>
    void operator()(GTy& graph, const GNode& src) const {
      Node& sdata = graph.getData(src, Galois::MethodFlag::UNPROTECTED);

      for (typename GTy::edge_iterator ii = graph.edge_begin(src, Galois::MethodFlag::UNPROTECTED),
          ei = graph.edge_end(src, Galois::MethodFlag::UNPROTECTED); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        Node& ddata = graph.getData(dst, Galois::MethodFlag::UNPROTECTED);

        if (symmetricGraph && src >= dst)
          continue;

        if (!sdata.merge(&ddata))
          emptyMerges += 1;
      }
    }
  };

  void operator()(Graph& graph) {
    Galois::Statistic emptyMerges("EmptyMerges");
    
    Galois::GraphChi::vertexMap(graph, Merge(emptyMerges), memoryLimit);
  }
};

/**
 * Like synchronous algorithm, but if we restrict path compression (as done is
 * @link{UnionFindNode}), we can perform unions and finds concurrently.
 */
struct AsyncAlgo {
  typedef Galois::Graph::LC_CSR_Graph<Node,void>
    ::with_numa_alloc<true>::type
    ::with_no_lockable<true>::type
    Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) { Galois::Graph::readGraph(graph, inputFilename); }

  struct Merge {
    typedef int tt_does_not_need_aborts;
    typedef int tt_does_not_need_push;

    Graph& graph;
    Galois::Statistic& emptyMerges;
    Merge(Graph& g, Galois::Statistic& e): graph(g), emptyMerges(e) { }

    //! Add the next edge between components to the worklist
    void operator()(const GNode& src, Galois::UserContext<GNode>&) const {
      (*this)(src);
    }

    void operator()(const GNode& src) const {
      Node& sdata = graph.getData(src, Galois::MethodFlag::UNPROTECTED);

      for (Graph::edge_iterator ii = graph.edge_begin(src, Galois::MethodFlag::UNPROTECTED),
          ei = graph.edge_end(src, Galois::MethodFlag::UNPROTECTED); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        Node& ddata = graph.getData(dst, Galois::MethodFlag::UNPROTECTED);

        if (symmetricGraph && src >= dst)
          continue;

        if (!sdata.merge(&ddata)) {
          emptyMerges += 1;
        } else if (traceWork) {
          *GoodWork += 1;
        }
      }
    }
  };

  void operator()(Graph& graph) {
    Galois::Statistic emptyMerges("EmptyMerges");
    Galois::for_each_local(graph, Merge(graph, emptyMerges));
  }
};

/**
 * Improve performance of async algorithm by following machine topology.
 */
struct BlockedAsyncAlgo {
  typedef Galois::Graph::LC_CSR_Graph<Node,void>
    ::with_numa_alloc<true>::type
    ::with_no_lockable<true>::type
    Graph;
  typedef Graph::GraphNode GNode;

  struct WorkItem {
    GNode src;
    Graph::edge_iterator start;
  };

  template<typename G>
  void readGraph(G& graph) { Galois::Graph::readGraph(graph, inputFilename); }

  struct Merge {
    typedef int tt_does_not_need_aborts;

    Graph& graph;
    Galois::InsertBag<WorkItem>& items;

    //! Add the next edge between components to the worklist
    template<bool MakeContinuation, int Limit, typename Pusher>
    void process(const GNode& src, const Graph::edge_iterator& start, Pusher& pusher) const {
      Node& sdata = graph.getData(src, Galois::MethodFlag::UNPROTECTED);
      int count = 1;
      for (Graph::edge_iterator ii = start, ei = graph.edge_end(src, Galois::MethodFlag::UNPROTECTED);
          ii != ei; 
          ++ii, ++count) {
        GNode dst = graph.getEdgeDst(ii);
        Node& ddata = graph.getData(dst, Galois::MethodFlag::UNPROTECTED);

        if (symmetricGraph && src >= dst)
          continue;

        if (sdata.merge(&ddata)) {
          if (Limit == 0 || count != Limit)
            continue;
        }

        if (MakeContinuation || (Limit != 0 && count == Limit)) {
          WorkItem item = { src, ii + 1 };
          pusher.push(item);
          break;
        }
      }
    }

    void operator()(const GNode& src) const {
      Graph::edge_iterator start = graph.edge_begin(src, Galois::MethodFlag::UNPROTECTED);
      if (Galois::Substrate::ThreadPool::getPackage() == 0) {
        process<true, 0>(src, start, items);
      } else {
        process<true, 1>(src, start, items);
      }
    }

    void operator()(const WorkItem& item, Galois::UserContext<WorkItem>& ctx) const {
      process<true, 0>(item.src, item.start, ctx);
    }
  };

  void operator()(Graph& graph) {
    Galois::InsertBag<WorkItem> items;
    Merge merge = { graph, items };
    Galois::do_all_local(graph, merge, Galois::loopname("Initialize"));
    Galois::for_each_local(items, merge,
        Galois::loopname("Merge"), Galois::wl<Galois::WorkList::dChunkedFIFO<128> >());
  }
};

template<typename Graph>
struct is_bad {
  typedef typename Graph::GraphNode GNode;
  Graph& graph;

  is_bad(Graph& g): graph(g) { }

  bool operator()(const GNode& n) const {
    typedef typename Graph::node_data_reference node_data_reference;

    node_data_reference me = graph.getData(n);
    for (typename Graph::edge_iterator ii = graph.edge_begin(n), ei = graph.edge_end(n); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      node_data_reference data = graph.getData(dst);
      if (data.component() != me.component()) {
        std::cerr << "not in same component: "
          << me.id << " (" << me.component() << ")"
          << " and "
          << data.id << " (" << data.component() << ")"
          << "\n";
        return true;
      }
    }
    return false;
  }
};

template<typename Graph>
bool verify(Graph& graph,
    typename std::enable_if<Galois::Graph::is_segmented<Graph>::value>::type* = 0) {
  return true;
}

template<typename Graph>
bool verify(Graph& graph,
    typename std::enable_if<!Galois::Graph::is_segmented<Graph>::value>::type* = 0) {
  return Galois::ParallelSTL::find_if(graph.begin(), graph.end(), is_bad<Graph>(graph)) == graph.end();
}

template<typename EdgeTy, typename Algo, typename CGraph>
void writeComponent(Algo& algo, CGraph& cgraph, typename CGraph::node_data_type::component_type component) {
  typedef typename CGraph::template with_edge_data<EdgeTy>::type Graph;

  if (std::is_same<Graph,CGraph>::value) {
    doWriteComponent(cgraph, component);
  } else {
    // copy node data from cgraph
    Graph graph;
    algo.readGraph(graph);
    typename Graph::iterator cc = graph.begin();
    for (typename CGraph::iterator ii = cgraph.begin(), ei = cgraph.end(); ii != ei; ++ii, ++cc) {
      graph.getData(*cc) = cgraph.getData(*ii);
    }
    doWriteComponent(graph, component);
  }
}

template<typename Graph>
void doWriteComponent(Graph& graph, typename Graph::node_data_type::component_type component,
    typename std::enable_if<Galois::Graph::is_segmented<Graph>::value>::type* = 0) {
  GALOIS_DIE("Writing component not supported for this graph");
}

template<typename Graph>
void doWriteComponent(Graph& graph, typename Graph::node_data_type::component_type component,
    typename std::enable_if<!Galois::Graph::is_segmented<Graph>::value>::type* = 0) {
  typedef typename Graph::GraphNode GNode;
  typedef typename Graph::node_data_reference node_data_reference;

  // set id to 1 if node is in component
  size_t numEdges = 0;
  size_t numNodes = 0;
  for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii) {
    node_data_reference data = graph.getData(*ii);
    data.id = data.component() == component ? 1 : 0;
    if (data.id) {
      size_t degree = std::distance(graph.edge_begin(*ii), graph.edge_end(*ii));
      numEdges += degree;
      numNodes += 1;
    }
  }

  typedef Galois::Graph::FileGraphWriter Writer;
  typedef Galois::LargeArray<typename Graph::edge_data_type> EdgeData;
  typedef typename EdgeData::value_type edge_value_type;

  Writer p;
  EdgeData edgeData;
  p.setNumNodes(numNodes);
  p.setNumEdges(numEdges);
  p.setSizeofEdgeData(EdgeData::has_value ? sizeof(edge_value_type) : 0); 
  edgeData.create(numEdges);

  p.phase1();
  // partial sums of ids: id == new_index + 1
  typename Graph::node_data_type* prev = 0;
  for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii) {
    node_data_reference data = graph.getData(*ii);
    if (prev)
      data.id = prev->id + data.id;
    if (data.component() == component) {
      size_t degree = std::distance(graph.edge_begin(*ii), graph.edge_end(*ii));
      size_t sid = data.id - 1;
      assert(sid < numNodes);
      p.incrementDegree(sid, degree);
    }
    
    prev = &data;
  }

  assert(!prev || prev->id == numNodes);

  if (largestComponentFilename != "") {
    p.phase2();
    for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii) {
      node_data_reference data = graph.getData(*ii);
      if (data.component() != component)
        continue;

      size_t sid = data.id - 1;

      for (typename Graph::edge_iterator jj = graph.edge_begin(*ii),
          ej = graph.edge_end(*ii); jj != ej; ++jj) {
        GNode dst = graph.getEdgeDst(jj);
        node_data_reference ddata = graph.getData(dst);
        size_t did = ddata.id - 1;

        //assert(ddata.component == component);
        assert(sid < numNodes && did < numNodes);
        if (EdgeData::has_value)
          edgeData.set(p.addNeighbor(sid, did), graph.getEdgeData(jj));
        else
          p.addNeighbor(sid, did);
      }
    }

    edge_value_type* rawEdgeData = p.finish<edge_value_type>();
    if (EdgeData::has_value)
      std::uninitialized_copy(std::make_move_iterator(edgeData.begin()), std::make_move_iterator(edgeData.end()), rawEdgeData);

    std::cout
      << "Writing largest component to " << largestComponentFilename
      << " (nodes: " << numNodes << " edges: " << numEdges << ")\n";

    p.toFile(largestComponentFilename);
  }

  if (permutationFilename != "") {
    std::ofstream out(permutationFilename);
    size_t oid = 0;
    std::cout << "Writing permutation to " << permutationFilename << "\n";
    for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii, ++oid) {
      node_data_reference data = graph.getData(*ii);
      out << oid << ",";
      if (data.component() != component) {
        ;
      } else {
        out << data.id - 1;
      }
      out << "\n";
    }
  }
}

template<typename Graph>
struct CountLargest {
  typedef typename Graph::node_data_type::component_type component_type;
  typedef std::map<component_type,int> Map;
  typedef typename Graph::GraphNode GNode;

  struct Accums {
    Galois::GMapElementAccumulator<Map> map;
    Galois::GAccumulator<size_t> reps;
  };

  Graph& graph;
  Accums& accums;
  
  CountLargest(Graph& g, Accums& accums): graph(g), accums(accums) { }
  
  void operator()(const GNode& x) const {
    typename Graph::node_data_reference n = graph.getData(x, Galois::MethodFlag::UNPROTECTED);
    if (n.isRep()) {
      accums.reps += 1;
      return;
    }

    // Don't add reps to table to avoid adding components of size 1
    accums.map.update(n.component(), 1);
  }
};

template<typename Graph>
struct ComponentSizePair {
  typedef typename Graph::node_data_type::component_type component_type;

  component_type component;
  int size;

  struct Max {
    ComponentSizePair operator()(const ComponentSizePair& a, const ComponentSizePair& b) const {
      if (a.size > b.size)
        return a;
      return b;
    }
  };

  ComponentSizePair(): component(0), size(0) { }
  ComponentSizePair(component_type c, int s): component(c), size(s) { }
};

template<typename Graph>
struct ReduceMax {
  typedef typename Graph::node_data_type::component_type component_type;
  typedef Galois::GSimpleReducible<ComponentSizePair<Graph>,typename ComponentSizePair<Graph>::Max> Accum;

  Accum& accum;

  ReduceMax(Accum& accum): accum(accum) { }

  void operator()(const std::pair<component_type,int>& x) const {
    accum.update(ComponentSizePair<Graph>(x.first, x.second));
  }
};

template<typename Graph>
typename Graph::node_data_type::component_type findLargest(Graph& graph) {
  typedef CountLargest<Graph> CL;
  typedef ReduceMax<Graph> RM;

  typename CL::Accums accums;
  Galois::do_all_local(graph, CL(graph, accums));
  typename CL::Map& map = accums.map.reduce();
  size_t reps = accums.reps.reduce();

  typename RM::Accum accumMax;
  Galois::do_all(map.begin(), map.end(), RM(accumMax));
  ComponentSizePair<Graph>& largest = accumMax.reduce();

  // Compensate for dropping representative node of components
  double ratio = graph.size() - reps + map.size();
  size_t largestSize = largest.size + 1;
  if (ratio)
    ratio = largestSize / ratio;

  std::cout << "Total components: " << reps << "\n";
  std::cout << "Number of non-trivial components: " << map.size()
    << " (largest size: " << largestSize << " [" << ratio << "])\n";

  return largest.component;
}

template<typename Algo>
void run() {
  typedef typename Algo::Graph Graph;

  Algo algo;
  Graph graph;

  algo.readGraph(graph);
  std::cout << "Read " << graph.size() << " nodes\n";

  unsigned int id = 0;
  for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii, ++id) {
    graph.getData(*ii).id = id;
  }
  
  Galois::preAlloc(numThreads + (2 * graph.size() * sizeof(typename Graph::node_data_type)) / Galois::Runtime::hugePageSize);
  Galois::reportPageAlloc("MeminfoPre");

  Galois::StatTimer T;
  T.start();
  algo(graph);
  T.stop();

  Galois::reportPageAlloc("MeminfoPost");

  if (!skipVerify || largestComponentFilename != "" || permutationFilename != "") {
    auto component = findLargest(graph);
    if (!verify(graph)) {
      GALOIS_DIE("verification failed");
    }
    if (component && (largestComponentFilename != "" || permutationFilename != "")) {
      switch (writeEdgeType) {
        case OutputEdgeType::void_: writeComponent<void>(algo, graph, component); break;
        case OutputEdgeType::int32_: writeComponent<uint32_t>(algo, graph, component); break;
        case OutputEdgeType::int64_: writeComponent<uint64_t>(algo, graph, component); break;
        default: abort();
      }
    }
  }
}

int main(int argc, char** argv) {
  Galois::StatManager statManager;
  LonestarStart(argc, argv, name, desc, url);

  if(traceWork) {
    GoodWork = new Galois::Statistic("GoodWork");
    EmptyWork = new Galois::Statistic("EmptyWork");
  }

  Galois::StatTimer T("TotalTime");
  T.start();
  switch (algo) {
    case Algo::asyncOc: run<AsyncOCAlgo>(); break;
    case Algo::async: run<AsyncAlgo>(); break;
    case Algo::blockedasync: run<BlockedAsyncAlgo>(); break;
    case Algo::labelProp: run<LabelPropAlgo<false> >(); break;
    case Algo::staleLp: run<LabelPropAlgo<true> >(); break;
    case Algo::pullLp: run<PullLPAlgo>(); break;
    case Algo::pullLpCas: run<PullLPCASAlgo>(); break;
    case Algo::serial: run<SerialAlgo>(); break;
    case Algo::synchronous: run<SynchronousAlgo>(); break;
#ifdef GALOIS_USE_EXP
    case Algo::graphchi: run<GraphChiAlgo>(); break;
    case Algo::graphlab: run<GraphLabAlgo>(); break;
    case Algo::ligraChi: run<LigraAlgo<true> >(); break;
    case Algo::ligra: run<LigraAlgo<false> >(); break;
#endif
    default: std::cerr << "Unknown algorithm\n"; abort();
  }
  T.stop();

  if(traceWork) {
    delete GoodWork;
    delete EmptyWork;
  }

  return 0;
}
