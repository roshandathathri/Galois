#ifndef EGONET_H
#define EGONET_H
#include "types.h"
#ifdef ALGO_EDGE
#define BOTTOM 1
#else
#define BOTTOM 2
#endif

class Egonet {
public:
	Egonet() {}
	Egonet(unsigned c, unsigned k) {
		allocate(c, k);
	}
	~Egonet() {}
	void allocate(unsigned c, unsigned k) {
		core = c;
		max_level = k;
		cur_level = BOTTOM;
		degrees.resize(k);
		sizes.resize(k);
		labels.resize(core);
		std::fill(sizes.begin(), sizes.end(), 0);
		std::fill(labels.begin(), labels.end(), 0);
		for (unsigned i = BOTTOM; i < k; i ++) degrees[i].resize(core);
		adj.resize(core*core);
	}
	void update(unsigned level, unsigned src) {
		unsigned begin = edge_begin(src);
		unsigned end = begin + get_degree(level-1, src);
		//unsigned end = egonet.edge_end(src);
		for (unsigned e = begin; e < end; e ++) {
			unsigned dst = getEdgeDst(e);
			if (get_label(dst) == level)
				inc_degree(level, src);
			else {
				set_adj(e--, getEdgeDst(--end));
				set_adj(end, dst);
			}
		}
	}
	size_t size() const { return sizes[cur_level]; }
	size_t size(unsigned level) const { return sizes[level]; }
	unsigned get_adj(unsigned vid) const { return adj[vid]; }
	unsigned getEdgeDst(unsigned vid) const { return adj[vid]; }
	unsigned get_degree(unsigned i) const { return degrees[cur_level][i]; }
	unsigned get_degree(unsigned level, unsigned i) const { return degrees[level][i]; }
	//unsigned get_cur_level() const { return cur_level; }
	BYTE get_label(unsigned vid) const { return labels[vid]; }
	void set_size(unsigned level, unsigned size) { sizes[level] = size; }
	void set_adj(unsigned vid, unsigned value) { adj[vid] = value; }
	void set_label(unsigned vid, BYTE value) { labels[vid] = value; }
	void set_degree(unsigned level, unsigned i, unsigned degree) { degrees[level][i] = degree; }
	void set_cur_level(unsigned level) { cur_level = level; }
	void inc_degree(unsigned level, unsigned i) { degrees[level][i] ++; }
	unsigned edge_begin(unsigned vid) { return vid * core; }
	unsigned edge_end(unsigned vid) { return vid * core + get_degree(vid); }
	//void increase_level() { cur_level ++; }

protected:
	unsigned core;
	unsigned max_level;
	UintList adj;//truncated list of neighbors
	IndexLists degrees;//degrees[level]: degrees of the vertices in the egonet
	UintList sizes; //sizes[level]: no. of embeddings (i.e. no. of vertices in the the current level)
	ByteList labels; //labels[i] is the label of each vertex i that shows its current level
	unsigned cur_level;
};

typedef galois::substrate::PerThreadStorage<Egonet> Egonets;

#endif
