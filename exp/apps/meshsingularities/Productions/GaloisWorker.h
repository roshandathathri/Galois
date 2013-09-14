#ifndef GALOISWORKER_H
#define GALOISWORKER_H

#include "GraphGenerator.hxx"
#include "Galois/Graph/LC_Morph_Graph.h"
#include "Galois/Runtime/ParallelWork.h"

#include <functional>
#include "TaskDescription.h"

#include "Galois/Statistic.h"
#include "Galois/Runtime/ll/SimpleLock.h"
#include "Galois/Runtime/ParallelWork.h"


typedef Galois::WorkList::dChunkedLIFO<1> WL;

struct ProductionProcess {
public:
	ProductionProcess() {};

	template<typename Context>
	void operator()(Graph::GraphNode src, Context& ctx);
	std::vector<double> *operator()(TaskDescription &td);

private:
	Graph *graph;
	int atomic_dec(int *value);
	//Galois::Runtime::PerPackageStorage<WL> pps;
	int leftRange(int tasks, int cpus, int i);
	int rightRange(int tasks, int cpus, int i);
};


#endif