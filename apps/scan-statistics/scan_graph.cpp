/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SA-GraphLib.
 *
 * SA-GraphLib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SA-GraphLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SA-GraphLib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <google/profiler.h>

#include <set>
#include <vector>

#include "thread.h"
#include "io_interface.h"

#include "graph_engine.h"
#include "graph_config.h"

#include "graphlab/cuckoo_set_pow2.hpp"

const double BIN_SEARCH_RATIO = 100;

struct timeval graph_start;
atomic_number<long> num_working_vertices;
atomic_number<long> num_completed_vertices;

class vertex_size_scheduler: public vertex_scheduler
{
	graph_engine *graph;
public:
	vertex_size_scheduler(graph_engine *graph) {
		this->graph = graph;
	}

	void schedule(std::vector<vertex_id_t> &vertices);
};

void vertex_size_scheduler::schedule(std::vector<vertex_id_t> &vertices)
{
	class vertex_size
	{
		vertex_id_t id;
		uint32_t size;
	public:
		vertex_size() {
			id = -1;
			size = 0;
		}

		void init(graph_engine *graph, vertex_id_t id) {
			this->id = id;
			this->size = graph->get_vertex(id).get_ext_mem_size();
		}

		uint32_t get_size() const {
			return size;
		}

		vertex_id_t get_id() const {
			return id;
		}
	};

	class comp_size
	{
	public:
		bool operator()(const vertex_size &v1, const vertex_size &v2) {
			return v1.get_size() > v2.get_size();
		}
	};

	std::vector<vertex_size> vertex_sizes(vertices.size());
	for (size_t i = 0; i < vertices.size(); i++)
		vertex_sizes[i].init(graph, vertices[i]);
	std::sort(vertex_sizes.begin(), vertex_sizes.end(), comp_size());
	for (size_t i = 0; i < vertices.size(); i++)
		vertices[i] = vertex_sizes[i].get_id();
}

class global_max
{
	volatile size_t value;
	pthread_spinlock_t lock;
public:
	global_max() {
		value = 0;
		pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
	}

	global_max(size_t init) {
		value = init;
		pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
	}

	bool update(size_t new_v) {
		if (new_v <= value)
			return false;

		bool ret = false;
		pthread_spin_lock(&lock);
		if (new_v > value) {
			value = new_v;
			ret = true;
		}
		pthread_spin_unlock(&lock);
		return ret;
	}

	size_t get() const {
		return value;
	}
} max_scan;

class count_msg: public vertex_message
{
	size_t num;
public:
	count_msg(size_t num): vertex_message(sizeof(count_msg), false) {
		this->num = num;
	}

	size_t get_num() const {
		return num;
	}
};

/**
 * The edge has two attributes:
 * The number of duplicated edges with the neighbor;
 * The number of edges that contributes to the neighbor's neighborhood.
 */
class attributed_neighbor
{
	vertex_id_t id;
	int num_dups;
	uint32_t *count;
public:
	attributed_neighbor() {
		id = -1;
		num_dups = 0;
		count = NULL;
	}

	attributed_neighbor(vertex_id_t id) {
		this->id = id;
		num_dups = 1;
		count = NULL;
	}

	attributed_neighbor(vertex_id_t id, int num_dups, uint32_t *count) {
		this->id = id;
		this->num_dups = num_dups;
		this->count = count;
	}

	bool is_valid() const {
		return count != NULL;
	}

	vertex_id_t get_id() const {
		return id;
	}

	int get_num_dups() const {
		return num_dups;
	}

	size_t get_count() const {
		return *count;
	}

	void inc_count(size_t count) {
		(*this->count) += count;
	}

	bool operator<(const attributed_neighbor &e) const {
		return this->id < e.id;
	}

	bool operator==(vertex_id_t id) const {
		return this->id == id;
	}

	bool operator==(const attributed_neighbor &e) const {
		return this->id == e.id;
	}
};

class comp_edge
{
public:
	bool operator()(const attributed_neighbor &e1,
			const attributed_neighbor &e2) {
		return e1.get_id() < e2.get_id();
	}
};

template<class InputIterator1, class InputIterator2, class Skipper,
	class Merger, class OutputIterator>
size_t unique_merge(InputIterator1 it1, InputIterator1 last1,
		InputIterator2 it2, InputIterator2 last2, Skipper skip,
		Merger merge, OutputIterator result)
{
	OutputIterator result_begin = result;
	while (it1 != last1 && it2 != last2) {
		if (*it2 < *it1) {
			typename std::iterator_traits<OutputIterator>::value_type v = *it2;
			++it2;
			while (it2 != last2 && v == *it2) {
				v = merge(v, *it2);
				++it2;
			}
			if (!skip(v))
				*(result++) = v;
		}
		else if (*it1 < *it2) {
			typename std::iterator_traits<OutputIterator>::value_type v = *it1;
			++it1;
			while (it1 != last1 && v == *it1) {
				v = merge(v, *it1);
				++it1;
			}
			if (!skip(v))
				*(result++) = v;
		}
		else {
			typename std::iterator_traits<OutputIterator>::value_type v = *it1;
			v = merge(v, *it2);
			++it2;
			while (it2 != last2 && v == *it2) {
				v = merge(v, *it2);
				++it2;
			}
			++it1;
			while (it1 != last1 && v == *it1) {
				v = merge(v, *it1);
				++it1;
			}
			if (!skip(v))
				*(result++) = v;
		}
	}

	while (it1 != last1) {
		typename std::iterator_traits<OutputIterator>::value_type v = *it1;
		++it1;
		while (it1 != last1 && v == *it1) {
			v = merge(v, *it1);
			++it1;
		}
		if (!skip(v))
			*(result++) = v;
	}

	while (it2 != last2) {
		typename std::iterator_traits<OutputIterator>::value_type v = *it2;
		++it2;
		while (it2 != last2 && v == *it2) {
			v = merge(v, *it2);
			++it2;
		}
		if (!skip(v))
			*(result++) = v;
	}
	return result - result_begin;
}

class neighbor_list
{
	class index_entry
	{
		vertex_id_t id;
		uint32_t idx;
	public:
		index_entry() {
			id = -1;
			idx = -1;
		}

		index_entry(vertex_id_t id) {
			this->id = id;
			this->idx = -1;
		}

		index_entry(vertex_id_t id, uint32_t idx) {
			this->id = id;
			this->idx = idx;
		}

		vertex_id_t get_id() const {
			return id;
		}

		uint32_t get_idx() const {
			return idx;
		}

		bool operator==(const index_entry &e) const {
			return id == e.get_id();
		}
	};

	class index_hash
	{
		boost::hash<vertex_id_t> id_hash;
	public:
		size_t operator()(const index_entry &e) const {
			return id_hash(e.get_id());
		}
	};

	typedef graphlab::cuckoo_set_pow2<index_entry, 3, size_t,
			index_hash> edge_set_t;

	class skip_larger {
		vertex_id_t id;
		size_t size;
		graph_engine &graph;
	public:
		skip_larger(graph_engine &_graph, vertex_id_t id): graph(_graph) {
			this->id = id;
			size = graph.get_vertex(id).get_ext_mem_size();
		}

		bool operator()(attributed_neighbor &e) {
			return operator()(e.get_id());
		}

		/**
		 * We are going to count edges on the vertices with the most edges.
		 * If two vertices have the same number of edges, we compute
		 * on the vertices with the largest Id.
		 */
		bool operator()(vertex_id_t id) {
			compute_vertex &info = graph.get_vertex(id);
			if (info.get_ext_mem_size() == size)
				return id >= this->id;
			return info.get_ext_mem_size() > size;
		}
	};

	class merge_edge {
	public:
		attributed_neighbor operator()(const attributed_neighbor &e1,
				const attributed_neighbor &e2) {
			assert(e1.get_id() == e2.get_id());
			return attributed_neighbor(e1.get_id(),
					e1.get_num_dups() + e2.get_num_dups(), NULL);
		}
	};

	std::vector<vertex_id_t> id_list;
	std::vector<int> num_dup_list;
	std::vector<uint32_t> count_list;
	edge_set_t *neighbor_set;

	// This helps to iterate on the id list.
	std::vector<vertex_id_t>::const_iterator it;
public:
	class id_iterator: public std::iterator<std::random_access_iterator_tag, vertex_id_t>
	{
		std::vector<vertex_id_t>::const_iterator it;
	public:
		typedef typename std::iterator<std::random_access_iterator_tag,
				vertex_id_t>::difference_type difference_type;

		id_iterator() {
		}

		id_iterator(const std::vector<vertex_id_t> &v) {
			it = v.begin();
		}

		difference_type operator-(const id_iterator &it) const {
			return this->it - it.it;
		}

		vertex_id_t operator*() const {
			return *it;
		}

		id_iterator &operator++() {
			it++;
			return *this;
		}

		id_iterator operator++(int) {
			id_iterator ret = *this;
			it++;
			return ret;
		}

		bool operator==(const id_iterator &it) const {
			return it.it == this->it;
		}
		
		bool operator!=(const id_iterator &it) const {
			return it.it != this->it;
		}

		id_iterator &operator+=(size_t num) {
			it += num;
			return *this;
		}
	};

	neighbor_list(graph_engine &graph, const page_vertex *vertex) {
		merge_edge merge;
		std::vector<attributed_neighbor> neighbors(
				vertex->get_num_edges(edge_type::BOTH_EDGES));
		size_t num_neighbors = unique_merge(
				vertex->get_neigh_begin(edge_type::IN_EDGE),
				vertex->get_neigh_end(edge_type::IN_EDGE),
				vertex->get_neigh_begin(edge_type::OUT_EDGE),
				vertex->get_neigh_end(edge_type::OUT_EDGE),
				skip_larger(graph, vertex->get_id()), merge,
				neighbors.begin());
		neighbors.resize(num_neighbors);
		id_list.resize(num_neighbors);
		num_dup_list.resize(num_neighbors);
		count_list.resize(num_neighbors);
		for (size_t i = 0; i < num_neighbors; i++) {
			id_list[i] = neighbors[i].get_id();
			num_dup_list[i] = neighbors[i].get_num_dups();
		}
		neighbor_set = NULL;
		if (num_neighbors > 0) {
			neighbor_set = new edge_set_t(index_entry(),
					0, 2 * neighbors.size());
			for (size_t i = 0; i < neighbors.size(); i++)
				neighbor_set->insert(index_entry(neighbors[i].get_id(), i));
		}

		start_id_iterator();
	}

	~neighbor_list() {
		if (neighbor_set)
			delete neighbor_set;
	}

	attributed_neighbor find(vertex_id_t id) {
		edge_set_t::const_iterator it = neighbor_set->find(id);
		if (it == neighbor_set->end())
			return attributed_neighbor();
		else {
			size_t idx = (*it).get_idx();
			assert(idx < id_list.size());
			return at(idx);
		}
	}

	attributed_neighbor at(size_t idx) {
		return attributed_neighbor(id_list[idx], num_dup_list[idx],
				&count_list[idx]);
	}

	bool contains(vertex_id_t id) {
		edge_set_t::const_iterator it = neighbor_set->find(id);
		return it != neighbor_set->end();
	}

	id_iterator get_id_begin() const {
		return id_iterator(id_list);
	}

	id_iterator get_id_end() const {
		id_iterator ret(id_list);
		ret += id_list.size();
		return ret;
	}

	size_t size() const {
		return id_list.size();
	}

	bool empty() const {
		return id_list.empty();
	}

	/**
	 * These methods are another way of iterating over the id list.
	 */

	bool has_next() const {
		return it != id_list.end();
	}

	vertex_id_t next() {
		vertex_id_t ret = *it;
		it++;
		return ret;
	}

	void start_id_iterator() {
		it = id_list.begin();
	}
};

/**
 * This data structure contains all data required when the vertex is
 * computing local scan.
 * It doesn't need to exist before or after the vertex computes local scan.
 */
struct runtime_data_t
{
	// All neighbors (in both in-edges and out-edges)
	neighbor_list neighbors;
	size_t est_local_scan;
	// The number of vertices that have joined with the vertex.
	unsigned num_joined;

	runtime_data_t(graph_engine &graph,
			const page_vertex *vertex): neighbors(graph, vertex) {
		est_local_scan = 0;
		num_joined = 0;
	}
};

class scan_vertex: public compute_vertex
{
	vsize_t num_in_edges;
	vsize_t num_out_edges;
	atomic_number<size_t> num_edges;
	runtime_data_t *data;

#ifdef PV_STAT
	// For testing
	size_t num_all_edges;
	size_t scan_bytes;
	size_t rand_jumps;
	size_t min_comps;
	long time_us;
	struct timeval vertex_start;
#endif
public:
	scan_vertex() {
		num_in_edges = 0;
		num_out_edges = 0;
		data = NULL;

#ifdef PV_STAT
		num_all_edges = 0;
		scan_bytes = 0;
		rand_jumps = 0;
		min_comps = 0;
		time_us = 0;
#endif
	}

	scan_vertex(vertex_id_t id, const vertex_index *index1): compute_vertex(
			id, index1) {
		const directed_vertex_index *index = (const directed_vertex_index *) index1;
		num_in_edges = index->get_num_in_edges(id);
		num_out_edges = index->get_num_out_edges(id);
		data = NULL;

#ifdef PV_STAT
		num_all_edges = 0;
		scan_bytes = 0;
		rand_jumps = 0;
		min_comps = 0;
		time_us = 0;
#endif
	}

#ifdef PV_STAT
	size_t get_scan_bytes() const {
		return scan_bytes;
	}

	size_t get_rand_jumps() const {
		return rand_jumps;
	}
#endif

	size_t get_result() const {
		return num_edges.get();
	}

	virtual bool has_required_vertices() const {
		if (data == NULL)
			return false;
		return data->neighbors.has_next();
	}

	virtual vertex_id_t get_next_required_vertex() {
		return data->neighbors.next();
	}

	size_t count_edges(graph_engine &graph, const page_vertex *v);
	size_t count_edges(graph_engine &graph, const page_vertex *v,
			edge_type type, std::vector<vertex_id_t> &common_neighs);
	size_t count_edges_hash(graph_engine &graph, const page_vertex *v,
			page_byte_array::const_iterator<vertex_id_t> other_it,
			page_byte_array::const_iterator<vertex_id_t> other_end,
			std::vector<vertex_id_t> &common_neighs);
	size_t count_edges_bin_search_this(graph_engine &graph, const page_vertex *v,
			neighbor_list::id_iterator this_it,
			neighbor_list::id_iterator this_end,
			page_byte_array::const_iterator<vertex_id_t> other_it,
			page_byte_array::const_iterator<vertex_id_t> other_end,
			std::vector<vertex_id_t> &common_neighs);
	size_t count_edges_bin_search_other(graph_engine &graph, const page_vertex *v,
			neighbor_list::id_iterator this_it,
			neighbor_list::id_iterator this_end,
			page_byte_array::const_iterator<vertex_id_t> other_it,
			page_byte_array::const_iterator<vertex_id_t> other_end,
			std::vector<vertex_id_t> &common_neighs);
	size_t count_edges_scan(graph_engine &graph, const page_vertex *v,
			neighbor_list::id_iterator this_it,
			neighbor_list::id_iterator this_end,
			page_byte_array::const_iterator<vertex_id_t> other_it,
			page_byte_array::const_iterator<vertex_id_t> other_end,
			std::vector<vertex_id_t> &common_neighs);

	bool run(graph_engine &graph) {
		size_t num_local_edges = num_in_edges + num_out_edges;
		return num_local_edges * num_local_edges >= max_scan.get();
	}

	bool run(graph_engine &graph, const page_vertex *vertex);

	bool run_on_neighbors(graph_engine &graph,
			const page_vertex *vertices[], int num);

	void run_on_messages(graph_engine &graph,
			const vertex_message *msgs[], int num) {
		for (int i = 0; i < num; i++) {
			const count_msg *msg = (const count_msg *) msgs[i];
			num_edges.inc(msg->get_num());
		}
		if (max_scan.update(num_edges.get())) {
			struct timeval curr;
			gettimeofday(&curr, NULL);
			printf("%d: new max scan: %ld at v%u\n",
					(int) time_diff(graph_start, curr),
					num_edges.get(), get_id());
		}
	}
};

size_t scan_vertex::count_edges_hash(graph_engine &graph, const page_vertex *v,
		page_byte_array::const_iterator<vertex_id_t> other_it,
		page_byte_array::const_iterator<vertex_id_t> other_end,
		std::vector<vertex_id_t> &common_neighs)
{
	size_t num_local_edges = 0;

	while (other_it != other_end) {
		vertex_id_t neigh_neighbor = *other_it;
		if (neigh_neighbor != v->get_id()
				&& neigh_neighbor != this->get_id()) {
			if (data->neighbors.contains(neigh_neighbor)) {
#if 0
				num_local_edges += (*other_data_it).get_count();
#endif
				num_local_edges++;
				common_neighs.push_back(neigh_neighbor);
			}
		}
		++other_it;
	}
	return num_local_edges;
}

#if 0
int scan_vertex::count_edges_bin_search_this(graph_engine &graph,
		const page_vertex *v,
		std::vector<attributed_neighbor>::const_iterator this_it,
		std::vector<attributed_neighbor>::const_iterator this_end,
		page_byte_array::const_iterator<vertex_id_t> other_it,
		page_byte_array::const_iterator<vertex_id_t> other_end,
		std::vector<vertex_id_t> &common_neighs)
{
	int num_local_edges = 0;
	int num_v_edges = other_end - other_it;
	int size_log2 = log2(neighbors->size());
	num_rand_jumps += size_log2 * num_v_edges;
	scan_bytes += num_v_edges * sizeof(vertex_id_t);
	while (other_it != other_end) {
		vertex_id_t neigh_neighbor = *other_it;
		if (neigh_neighbor != v->get_id()
				&& neigh_neighbor != this->get_id()) {
			std::vector<attributed_neighbor>::const_iterator first
				= std::lower_bound(this_it, this_end,
						attributed_neighbor(neigh_neighbor), comp_edge());
			if (first != this_end && neigh_neighbor == first->get_id()) {
#if 0
				num_local_edges += (*other_data_it).get_count();
#endif
				num_local_edges++;
				common_neighs.push_back(first->get_id());
			}
		}
		++other_it;
#if 0
		++other_data_it;
#endif
	}
	return num_local_edges;
}
#endif

size_t scan_vertex::count_edges_bin_search_other(graph_engine &graph,
		const page_vertex *v,
		neighbor_list::id_iterator this_it,
		neighbor_list::id_iterator this_end,
		page_byte_array::const_iterator<vertex_id_t> other_it,
		page_byte_array::const_iterator<vertex_id_t> other_end,
		std::vector<vertex_id_t> &common_neighs)
{
	size_t num_local_edges = 0;

	for (; this_it != this_end; this_it++) {
		vertex_id_t this_neighbor = *this_it;
		// We need to skip loops.
		if (this_neighbor == v->get_id()
				|| this_neighbor == this->get_id()) {
			continue;
		}

		page_byte_array::const_iterator<vertex_id_t> first
			= std::lower_bound(other_it, other_end, this_neighbor);
		// found it.
		if (first != other_end && !(this_neighbor < *first)) {
			int num_dups = 0;
			do {
#if 0
				page_byte_array::const_iterator<edge_count> data_it
					= other_data_it;
				data_it += first - other_it;
				// Edges in the v's neighbor lists may duplicated.
				// The duplicated neighbors need to be counted
				// multiple times.
				num_local_edges += (*data_it).get_count();
				++data_it;
#endif
				num_dups++;
				num_local_edges++;
				++first;
			} while (first != other_end && this_neighbor == *first);
			common_neighs.push_back(this_neighbor);
		}
	}
	return num_local_edges;
}

size_t scan_vertex::count_edges_scan(graph_engine &graph, const page_vertex *v,
		neighbor_list::id_iterator this_it,
		neighbor_list::id_iterator this_end,
		page_byte_array::const_iterator<vertex_id_t> other_it,
		page_byte_array::const_iterator<vertex_id_t> other_end,
		std::vector<vertex_id_t> &common_neighs)
{
	size_t num_local_edges = 0;
	while (other_it != other_end && this_it != this_end) {
		vertex_id_t this_neighbor = *this_it;
		vertex_id_t neigh_neighbor = *other_it;
		if (neigh_neighbor == v->get_id()
				|| neigh_neighbor == this->get_id()) {
			++other_it;
#if 0
			++other_data_it;
#endif
			continue;
		}
		if (this_neighbor == neigh_neighbor) {
			common_neighs.push_back(*this_it);
			do {
				// Edges in the v's neighbor lists may duplicated.
				// The duplicated neighbors need to be counted
				// multiple times.
#if 0
				num_local_edges += (*other_data_it).get_count();
				++other_data_it;
#endif
				num_local_edges++;
				++other_it;
			} while (other_it != other_end && this_neighbor == *other_it);
			++this_it;
		}
		else if (this_neighbor < neigh_neighbor) {
			++this_it;
		}
		else {
			++other_it;
#if 0
			++other_data_it;
#endif
		}
	}
	return num_local_edges;
}

size_t scan_vertex::count_edges(graph_engine &graph, const page_vertex *v,
		edge_type type, std::vector<vertex_id_t> &common_neighs)
{
	size_t num_v_edges = v->get_num_edges(type);
	if (num_v_edges == 0)
		return 0;

#ifdef PV_STAT
	min_comps += min(num_v_edges, data->neighbors.size());
#endif
	page_byte_array::const_iterator<vertex_id_t> other_it
		= v->get_neigh_begin(type);
#if 0
	page_byte_array::const_iterator<edge_count> other_data_it
		= v->get_edge_data_begin<edge_count>(type);
#endif
	page_byte_array::const_iterator<vertex_id_t> other_end
		= std::lower_bound(other_it, v->get_neigh_end(type),
				v->get_id());
	num_v_edges = other_end - other_it;
	if (num_v_edges == 0)
		return 0;

	neighbor_list::id_iterator this_it = data->neighbors.get_id_begin();
	neighbor_list::id_iterator this_end = data->neighbors.get_id_end();
	this_end = std::lower_bound(this_it, this_end,
			v->get_id(), comp_edge());

	if (num_v_edges / data->neighbors.size() > BIN_SEARCH_RATIO) {
#ifdef PV_STAT
		int size_log2 = log2(num_v_edges);
		scan_bytes += data->neighbors.size() * sizeof(vertex_id_t);
		rand_jumps += size_log2 * data->neighbors.size();
#endif
		return count_edges_bin_search_other(graph, v, this_it, this_end,
				other_it, other_end, common_neighs);
	}
	else if (data->neighbors.size() / num_v_edges > 4) {
#ifdef PV_STAT
		scan_bytes += num_v_edges * sizeof(vertex_id_t);
		rand_jumps += num_v_edges;
#endif
		return count_edges_hash(graph, v, other_it, other_end, common_neighs);
	}
	else {
#ifdef PV_STAT
		scan_bytes += num_v_edges * sizeof(vertex_id_t);
		scan_bytes += data->neighbors.size() * sizeof(vertex_id_t);
#endif
		return count_edges_scan(graph, v, this_it, this_end,
				other_it, other_end, common_neighs);
	}
}

size_t scan_vertex::count_edges(graph_engine &graph, const page_vertex *v)
{
	assert(!data->neighbors.empty());
	if (v->get_num_edges(edge_type::BOTH_EDGES) == 0)
		return 0;

	std::vector<vertex_id_t> common_neighs1;
	std::vector<vertex_id_t> common_neighs2;
	size_t ret = count_edges(graph, v, edge_type::IN_EDGE, common_neighs1)
		+ count_edges(graph, v, edge_type::OUT_EDGE, common_neighs2);

	class skip_self {
	public:
		bool operator()(vertex_id_t id) {
			return false;
		}
	};

	class merge_edge {
	public:
		vertex_id_t operator()(vertex_id_t id1, vertex_id_t id2) {
			assert(id1 == id2);
			return id1;
		}
	};

	std::vector<vertex_id_t> common_neighs(common_neighs1.size()
			+ common_neighs2.size());
	size_t num_neighbors = unique_merge(
			common_neighs1.begin(), common_neighs1.end(),
			common_neighs2.begin(), common_neighs2.end(),
			skip_self(), merge_edge(), common_neighs.begin());
	common_neighs.resize(num_neighbors);

#ifdef PV_STAT
	rand_jumps += common_neighs.size() + 1;
#endif
	// The number of duplicated edges between v and this vertex.
	attributed_neighbor neigh = data->neighbors.find(v->get_id());
	assert(neigh.is_valid());
	int num_v_dups = neigh.get_num_dups();
	assert(num_v_dups > 0);
	size_t num_edges = 0;
	for (std::vector<vertex_id_t>::const_iterator it = common_neighs.begin();
			it != common_neighs.end(); it++) {
		attributed_neighbor n = data->neighbors.find(*it);
		assert(n.is_valid());
		num_edges += n.get_num_dups();
		n.inc_count(num_v_dups);
	}
	if (num_edges > 0)
		neigh.inc_count(num_edges);
	return ret;
}

bool scan_vertex::run(graph_engine &graph, const page_vertex *vertex)
{
	assert(data == NULL);

	size_t num_local_edges = vertex->get_num_edges(edge_type::BOTH_EDGES);
	assert(num_local_edges == (size_t) num_in_edges + num_out_edges);
#ifdef PV_STAT
	num_all_edges = num_local_edges;
#endif
	if (num_local_edges == 0)
		return true;

	class skip_self {
		vertex_id_t id;
	public:
		skip_self(vertex_id_t id) {
			this->id = id;
		}

		bool operator()(vertex_id_t id) {
			return this->id == id;
		}
	};

	class merge_edge {
	public:
		vertex_id_t operator()(vertex_id_t e1, vertex_id_t e2) {
			assert(e1 == e2);
			return e1;
		}
	};

	std::vector<vertex_id_t> all_neighbors(
			vertex->get_num_edges(edge_type::BOTH_EDGES));
	size_t num_neighbors = unique_merge(
			vertex->get_neigh_begin(edge_type::IN_EDGE),
			vertex->get_neigh_end(edge_type::IN_EDGE),
			vertex->get_neigh_begin(edge_type::OUT_EDGE),
			vertex->get_neigh_end(edge_type::OUT_EDGE),
			skip_self(vertex->get_id()), merge_edge(),
			all_neighbors.begin());
	all_neighbors.resize(num_neighbors);

	size_t tot_edges = num_local_edges;
	for (size_t i = 0; i < all_neighbors.size(); i++) {
		scan_vertex &v = (scan_vertex &) graph.get_vertex(all_neighbors[i]);
		// The max number of common neighbors should be smaller than all neighbors
		// in the neighborhood, assuming there aren't duplicated edges.
		tot_edges += min(v.num_in_edges + v.num_out_edges, num_neighbors * 2);
	}
	tot_edges /= 2;
	if (tot_edges < max_scan.get())
		return true;

	long ret = num_working_vertices.inc(1);
	if (ret % 100000 == 0)
		printf("%ld working vertices\n", ret);

	data = new runtime_data_t(graph, vertex);
	data->est_local_scan = tot_edges;
#ifdef PV_STAT
	gettimeofday(&vertex_start, NULL);
	fprintf(stderr, "compute v%u (with %d edges, compute on %ld edges, potential %ld inter-edges) on thread %d at %.f seconds\n",
			get_id(), num_all_edges, data->neighbors.size(), tot_edges, thread::get_curr_thread()->get_id(),
			time_diff(graph_start, vertex_start));
#endif

	page_byte_array::const_iterator<vertex_id_t> it = vertex->get_neigh_begin(
			edge_type::BOTH_EDGES);
#if 0
	page_byte_array::const_iterator<edge_count> data_it
		= vertex->get_edge_data_begin<edge_count>(
				edge_type::BOTH_EDGES);
#endif
	page_byte_array::const_iterator<vertex_id_t> end = vertex->get_neigh_end(
			edge_type::BOTH_EDGES);
#if 0
	for (; it != end; ++it, ++data_it) {
#endif
	size_t tmp = 0;
	for (; it != end; ++it) {
		vertex_id_t id = *it;
		// Ignore loops
		if (id != vertex->get_id()) {
#if 0
			num_edges.inc((*data_it).get_count());
#endif
			tmp++;
		}
	}
	num_edges.inc(tmp);

	if (data->neighbors.empty()) {
		delete data;
		data = NULL;
		long ret = num_completed_vertices.inc(1);
		if (ret % 100000 == 0)
			printf("%ld completed vertices\n", ret);
		return true;
	}

	return false;
}

bool scan_vertex::run_on_neighbors(graph_engine &graph,
		const page_vertex *vertices[], int num)
{
	assert(data);
	data->num_joined += num;
	if (data->est_local_scan < max_scan.get())
		return data->num_joined == data->neighbors.size();
#ifdef PV_STAT
	struct timeval start, end;
	gettimeofday(&start, NULL);
#endif
	for (int i = 0; i < num; i++) {
		size_t ret = count_edges(graph, vertices[i]);
		if (ret > 0)
			num_edges.inc(ret);
	}
#ifdef PV_STAT
	gettimeofday(&end, NULL);
	time_us += time_diff_us(start, end);
#endif

	// If we have seen all required neighbors, we have complete
	// the computation. We can release the memory now.
	if (data->num_joined == data->neighbors.size()) {
		if (max_scan.update(num_edges.get())) {
			struct timeval curr;
			gettimeofday(&curr, NULL);
			printf("%d: new max scan: %ld at v%u\n",
					(int) time_diff(graph_start, curr),
					num_edges.get(), get_id());
		}
		long ret = num_completed_vertices.inc(1);
		if (ret % 100000 == 0)
			printf("%ld completed vertices\n", ret);

#ifdef PV_STAT
		struct timeval curr;
		gettimeofday(&curr, NULL);
		fprintf(stderr,
				"v%u: # edges: %d, scan: %d, scan bytes: %ld, # rand jumps: %ld, # comps: %ld, time: %ldms\n",
				get_id(), num_all_edges, num_edges.get(), scan_bytes, rand_jumps, min_comps, time_us / 1000);
#endif

		// Inform all neighbors in the in-edges.
		for (size_t i = 0; i < data->neighbors.size(); i++) {
			size_t count = data->neighbors.at(i).get_count();
			if (count > 0) {
				count_msg msg(count);
				graph.send_msg(data->neighbors.at(i).get_id(), msg);
			}
		}

		delete data;
		data = NULL;
		return true;
	}
	return false;
}

void int_handler(int sig_num)
{
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	exit(0);
}

void print_usage()
{
	fprintf(stderr,
			"scan-statistics [options] conf_file graph_file index_file\n");
	fprintf(stderr, "-o file: output local scan of each vertex to a file\n");
	fprintf(stderr, "-c confs: add more configurations to the system\n");
	graph_conf.print_help();
	params.print_help();
}

int main(int argc, char *argv[])
{
	int opt;
	std::string output_file;
	std::string confs;
	int num_opts = 0;
	while ((opt = getopt(argc, argv, "o:c:")) != -1) {
		num_opts++;
		switch (opt) {
			case 'o':
				output_file = optarg;
				num_opts++;
				break;
			case 'c':
				confs = optarg;
				num_opts++;
				break;
			default:
				print_usage();
		}
	}
	argv += 1 + num_opts;
	argc -= 1 + num_opts;

	if (argc < 3) {
		print_usage();
		exit(-1);
	}

	std::string conf_file = argv[0];
	std::string graph_file = argv[1];
	std::string index_file = argv[2];

	config_map configs(conf_file);
	configs.add_options(confs);
	graph_conf.init(configs);
	graph_conf.print();

	signal(SIGINT, int_handler);
	init_io_system(configs);

	graph_index *index = NUMA_graph_index<scan_vertex>::create(
			index_file, graph_conf.get_num_threads(), params.get_num_nodes());
	graph_engine *graph = graph_engine::create(
			graph_conf.get_num_threads(), params.get_num_nodes(), graph_file,
			index);
	// Let's schedule the order of processing activated vertices according
	// to the size of vertices. We start with processing vertices with higher
	// degrees in the hope we can find the max scan as early as possible,
	// so that we can simple ignore the rest of vertices.
	graph->set_vertex_scheduler(new vertex_size_scheduler(graph));
	// TODO I need to redefine this interface.
	graph->set_required_neighbor_type(edge_type::BOTH_EDGES);
	printf("scan statistics starts\n");
	printf("prof_file: %s\n", graph_conf.get_prof_file().c_str());
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());

	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph_start = start;
	graph->start_all();
	graph->wait4complete();
	gettimeofday(&end, NULL);

	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	if (graph_conf.get_print_io_stat())
		print_io_thread_stat();
	graph_engine::destroy(graph);
	printf("It takes %f seconds\n", time_diff(start, end));
	printf("There are %ld vertices\n", index->get_num_vertices());
	printf("process %ld vertices and complete %ld vertices\n",
			num_working_vertices.get(), num_completed_vertices.get());
	printf("global max scan: %ld\n", max_scan.get());

#ifdef PV_STAT
	graph_index::const_iterator it = index->begin();
	graph_index::const_iterator end_it = index->end();
	size_t tot_scan_bytes = 0;
	size_t tot_rand_jumps = 0;
	for (; it != end_it; ++it) {
		const scan_vertex &v = (const scan_vertex &) *it;
		tot_scan_bytes += v.get_scan_bytes();
		tot_rand_jumps += v.get_rand_jumps();
	}
	printf("scan %ld bytes, %ld rand jumps\n",
			tot_scan_bytes, tot_rand_jumps);
#endif

	if (!output_file.empty()) {
		FILE *f = fopen(output_file.c_str(), "w");
		if (f == NULL) {
			perror("fopen");
			return -1;
		}
		graph_index::const_iterator it = index->begin();
		graph_index::const_iterator end_it = index->end();
		for (; it != end_it; ++it) {
			const scan_vertex &v = (const scan_vertex &) *it;
			fprintf(f, "\"%ld\" %ld\n", (unsigned long) v.get_id(), v.get_result());
		}
		fclose(f);
	}
}