#include <unordered_set>
#include "mapper.hpp"
#include "timer.hpp"

//#define debug_mapper

namespace vg {

Mapper::Mapper(Index* idex,
               xg::XG* xidex,
               gcsa::GCSA* g,
               gcsa::LCPArray* a)
    : index(idex)
    , xindex(xidex)
    , gcsa(g)
    , lcp(a)
    , best_clusters(0)
    , cluster_min(1)
    , hit_max(0)
    , hit_size_threshold(512)
    , kmer_min(0)
    , kmer_sensitivity_step(5)
    , thread_extension(10)
    , max_thread_gap(30)
    , context_depth(1)
    , max_multimaps(1)
    , max_attempts(0)
    , softclip_threshold(0)
    , max_softclip_iterations(10)
    , prefer_forward(false)
    , greedy_accept(false)
    , accept_identity(0.75)
    , min_identity(0)
    , min_kmer_entropy(0)
    , debug(false)
    , alignment_threads(1)
    , min_mem_length(0)
    , mem_chaining(false)
    , fast_reseed(true)
    , max_target_factor(128)
    , max_query_graph_ratio(128)
    , extra_multimaps(100)
    , always_rescue(false)
    , fragment_size(0)
    , fragment_max(1e5)
    , fragment_sigma(4)
    , fragment_length_cache_size(1000)
    , cached_fragment_length_mean(0)
    , cached_fragment_length_stdev(0)
    , cached_fragment_orientation(0)
    , cached_fragment_direction(1)
    , since_last_fragment_length_estimate(0)
    , fragment_length_estimate_interval(10)
    , perfect_pair_identity_threshold(0.98)
    , mapping_quality_method(Approx)
    , adjust_alignments_for_base_quality(false)
    , max_mapping_quality(60)
    , max_cluster_mapping_quality(1024)
    , mem_reseed_length(0)
    , use_cluster_mq(false)
    , smooth_alignments(true)
    , simultaneous_pair_alignment(true)
    , drop_chain(0.2)
    , mq_overlap(0.2)
    , cache_size(128)
    , mate_rescues(32)
    , alignment_match(1)
    , alignment_mismatch(4)
    , alignment_gap_open(6)
    , alignment_gap_extension(1)
    , full_length_alignment_bonus(5)
{
    init_aligner(alignment_match, alignment_mismatch, alignment_gap_open, alignment_gap_extension);
    init_node_cache();
    init_node_start_cache();
    init_node_pos_cache();
    init_edge_cache();
}

Mapper::Mapper(Index* idex, gcsa::GCSA* g, gcsa::LCPArray* a) : Mapper(idex, nullptr, g, a)
{
    if(idex == nullptr) {
        // With this constructor we need an index.
        cerr << "error:[vg::Mapper] cannot create a RocksDB-based Mapper with null index" << endl;
        exit(1);
    }

    kmer_sizes = index->stored_kmer_sizes();
    if (kmer_sizes.empty() && gcsa == NULL) {
        cerr << "error:[vg::Mapper] the index ("
             << index->name << ") does not include kmers"
             << " and no GCSA index has been provided" << endl;
        exit(1);
    }
}

Mapper::Mapper(xg::XG* xidex, gcsa::GCSA* g, gcsa::LCPArray* a) : Mapper(nullptr, xidex, g, a) {
    if(xidex == nullptr) {
        // With this constructor we need an XG graph.
        cerr << "error:[vg::Mapper] cannot create an xg-based Mapper with null xg index" << endl;
        exit(1);
    }

    if(g == nullptr || a == nullptr) {
        // With this constructor we need a GCSA2 index too.
        cerr << "error:[vg::Mapper] cannot create an xg-based Mapper with null GCSA2 index" << endl;
        exit(1);
    }
}

Mapper::Mapper(void) : Mapper(nullptr, nullptr, nullptr, nullptr) {
    // Nothing to do. Default constructed and can't really do anything.
}

Mapper::~Mapper(void) {
    for (auto& aligner : qual_adj_aligners) {
        delete aligner;
    }
    for (auto& aligner : regular_aligners) {
        delete aligner;
    }
    for (auto& nc : node_cache) {
        delete nc;
    }
    for (auto& np : node_pos_cache) {
        delete np;
    }
}
    
double Mapper::estimate_gc_content(void) {
    
    uint64_t at = 0, gc = 0;
    
    if (gcsa) {
        at = gcsa::Range::length(gcsa->find(string("A"))) + gcsa::Range::length(gcsa->find(string("T")));
        gc = gcsa::Range::length(gcsa->find(string("G"))) + gcsa::Range::length(gcsa->find(string("C")));
    }
    else if (index) {
        at = index->approx_size_of_kmer_matches("A") + index->approx_size_of_kmer_matches("T");
        gc = index->approx_size_of_kmer_matches("G") + index->approx_size_of_kmer_matches("C");
    }

    if (at == 0 || gc == 0) {
        return default_gc_content;
    }
    
    return ((double) gc) / (at + gc);
}

int Mapper::random_match_length(double chance_random) {
    size_t length = 0;
    if (xindex) {
        length = xindex->seq_length;
    } else if (index) {
        length = index->approx_size_of_kmer_matches("");
    } else {
        return 0;
    }
    return ceil(- (log(1.0 - pow(pow(1.0-chance_random, -1), (-1.0/length))) / log(4.0)));
}

double Mapper::graph_entropy(void) {
    const size_t seq_bytes = xindex->sequence_bit_size() / 8;
    char* seq = (char*) xindex->sequence_data();
    return entropy(seq, seq_bytes);
}

void Mapper::set_alignment_threads(int new_thread_count) {
    alignment_threads = new_thread_count;
    clear_aligners(); // number of aligners per mapper depends on thread count
    init_aligner(alignment_match, alignment_mismatch, alignment_gap_open, alignment_gap_extension);
    init_node_cache();
    init_node_start_cache();
    init_node_pos_cache();
    init_edge_cache();
}

void Mapper::init_node_cache(void) {
    for (auto& nc : node_cache) {
        delete nc;
    }
    node_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        node_cache.push_back(new LRUCache<id_t, Node>(cache_size));
    }
}

void Mapper::init_node_start_cache(void) {
    for (auto& nc : node_start_cache) {
        delete nc;
    }
    node_start_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        node_start_cache.push_back(new LRUCache<id_t, size_t>(cache_size));
    }
}

void Mapper::init_node_pos_cache(void) {
    for (auto& nc : node_pos_cache) {
        delete nc;
    }
    node_pos_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        node_pos_cache.push_back(new LRUCache<gcsa::node_type, map<string, vector<size_t> > >(cache_size));
    }
}

void Mapper::init_edge_cache(void) {
    for (auto& ec : edge_cache) {
        delete ec;
    }
    edge_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        edge_cache.push_back(new LRUCache<id_t, vector<Edge> >(cache_size));
    }
}

void Mapper::clear_aligners(void) {
    for (auto& aligner : qual_adj_aligners) {
        delete aligner;
    }
    qual_adj_aligners.clear();
    for (auto& aligner : regular_aligners) {
        delete aligner;
    }
    regular_aligners.clear();
}

void Mapper::init_aligner(int8_t match, int8_t mismatch, int8_t gap_open, int8_t gap_extend) {
    // hacky, find max score so that scaling doesn't change score
    int8_t max_score = match;
    if (mismatch > max_score) max_score = mismatch;
    if (gap_open > max_score) max_score = gap_open;
    if (gap_extend > max_score) max_score = gap_extend;
    
    double gc_content = estimate_gc_content();

    for (int i = 0; i < alignment_threads; ++i) {
        qual_adj_aligners.push_back(new QualAdjAligner(match, mismatch, gap_open, gap_extend, max_score, 255, gc_content));
        regular_aligners.push_back(new Aligner(match, mismatch, gap_open, gap_extend));
    }
}

void Mapper::set_alignment_scores(int8_t match, int8_t mismatch, int8_t gap_open, int8_t gap_extend) {
    alignment_match = match;
    alignment_mismatch = mismatch;
    alignment_gap_open = gap_open;
    alignment_gap_extension = gap_extend;
    if (!qual_adj_aligners.empty()
        && !regular_aligners.empty()) {
        auto aligner = regular_aligners.front();
        // we've already set the right score
        if (match == aligner->match && mismatch == aligner->mismatch &&
            gap_open == aligner->gap_open && gap_extend == aligner->gap_extension) {
            return;
        }
        // otherwise, destroy them and reset
        clear_aligners();
    }
    // reset the aligners
    init_aligner(match, mismatch, gap_open, gap_extend);
}

// todo add options for aligned global and pinned
Alignment Mapper::align_to_graph(const Alignment& aln,
                                 VG& vg,
                                 size_t max_query_graph_ratio,
                                 bool pinned_alignment,
                                 bool pin_left,
                                 int8_t full_length_bonus,
                                 bool banded_global) {
    // check if we have a cached aligner for this thread
    if (aln.quality().empty() || !adjust_alignments_for_base_quality) {
        Aligner* aligner = get_regular_aligner();
        //aligner.align_global_banded(aln, graph.graph, band_padding);
        return vg.align(aln,
                        aligner,
                        max_query_graph_ratio,
                        pinned_alignment,
                        pin_left,
                        full_length_bonus,
                        banded_global,
                        0, // band padding override
                        aln.sequence().size());
    } else {
        QualAdjAligner* aligner = get_qual_adj_aligner();
        return vg.align_qual_adjusted(aln,
                                      aligner,
                                      max_query_graph_ratio,
                                      pinned_alignment,
                                      pin_left,
                                      full_length_bonus,
                                      banded_global,
                                      0, // band padding override
                                      aln.sequence().size());
    }
}

Alignment Mapper::align(const string& seq, int kmer_size, int stride, int max_mem_length, int band_width) {
    Alignment aln;
    aln.set_sequence(seq);
    return align(aln, kmer_size, stride, max_mem_length, band_width);
}

// align read2 near read1's mapping location
void Mapper::align_mate_in_window(const Alignment& read1, Alignment& read2, int pair_window) {
    if (read1.score() == 0) return; // bail out if we haven't aligned the first
    // try to recover in region
    auto& path = read1.path();
    int64_t idf = path.mapping(0).position().node_id();
    int64_t idl = path.mapping(path.mapping_size()-1).position().node_id();
    if(idf > idl) {
        swap(idf, idl);
    }
    // but which way should we expand? this will make things much easier.
    
    // We'll look near the leftmost and rightmost nodes, but we won't try and
    // bridge the whole area of the read, because there may be an ID
    // discontinuity.
    int64_t first = max((int64_t)0, idf - pair_window);
    int64_t last = idl + (int64_t) pair_window;
    
    // Now make sure the ranges don't overlap, because if they do we'll
    // duplicate nodes.
    
    // They can only overlap as idf on top of idl, since we swapped them above.
    // TODO: account at all for orientation? Maybe our left end is in higher
    // numbers but really does need to look left and not right.
    if(idf >= idl) {
        idf--;
    }
    
    VG* graph = new VG;

    if(debug) {
        cerr << "Rescuing in " << first << "-" << idf << " and " << idl << "-" << last << endl;
    }
    
    // TODO: how do we account for orientation when using ID ranges?

    // Now we need to get the neighborhood by ID and expand outward by actual
    // edges. How we do this depends on what indexing structures we have.
    if(xindex) {
        // should have callback here
        xindex->get_id_range(first, idf, graph->graph);
        xindex->get_id_range(idl, last, graph->graph);
        
        // don't get the paths (this isn't yet threadsafe in sdsl-lite)
        xindex->expand_context(graph->graph, context_depth, false);
        graph->rebuild_indexes();
    } else if(index) {
        index->get_range(first, idf, *graph);
        index->get_range(idl, last, *graph);
        index->expand_context(*graph, context_depth);
    } else {
        cerr << "error:[vg::Mapper] cannot align mate with no graph data" << endl;
        exit(1);
    }


    graph->remove_orphan_edges();
    
    if(debug) {
        cerr << "Rescue graph size: " << graph->size() << endl;
    }
    
    read2.clear_path();
    read2.set_score(0);

    read2 = align_to_graph(read2, *graph, max_query_graph_ratio);
    delete graph;
}

map<string, double> Mapper::alignment_mean_path_positions(const Alignment& aln, bool first_hit_only) {
    map<string, double> mean_pos;
    // Alignments are consistent if their median node id positions are within the fragment_size
    
    // We need the sets of nodes visited by each alignment
    set<id_t> ids;
    
    for(size_t i = 0; i < aln.path().mapping_size(); i++) {
        // Collect all the unique nodes visited by the first algnment
        ids.insert(aln.path().mapping(i).position().node_id());
    }
    map<string, map<int, vector<id_t> > > node_positions;
    for(auto id : ids) {
        for (auto& ref : node_positions_in_paths(gcsa::Node::encode(id, 0))) {
            auto& name = ref.first;
            for (auto pos : ref.second) {
                node_positions[name][pos].push_back(id);
            }
        }
        // just get the first one
        if (first_hit_only && node_positions.size()) break;
    }
    // get median mapping positions
    int idscount = 0;
    double idssum = 0;
    for (auto& ref : node_positions) {
        for (auto& p : ref.second) {
            for (auto& n : p.second) {
                auto pos = p.first + get_node_length(n)/2;
                if (ids.count(n)) {
                    idscount++;
                    idssum += pos;
                }
            }
        }
        mean_pos[ref.first] = idssum/idscount;
    }
    return mean_pos;
}

pos_t Mapper::likely_mate_position(const Alignment& aln, bool is_first_mate) {
    bool aln_is_rev = aln.path().mapping(0).position().is_reverse();
    int aln_pos = approx_alignment_position(aln);
    bool same_orientation = cached_fragment_orientation;
    bool forward_direction = cached_fragment_direction;
    int delta = cached_fragment_length_mean;
    // which way is our delta?
    // we are on the forward strand
    id_t target;
    if (forward_direction) {
        if (is_first_mate) {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos + delta);
            } else {
                target = node_approximately_at(aln_pos - delta);
            }
        } else {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos + delta);
            } else {
                target = node_approximately_at(aln_pos - delta);
            }
        }
    } else {
        if (is_first_mate) {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos - delta);
            } else {
                target = node_approximately_at(aln_pos + delta);
            }
        } else {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos - delta);
            } else {
                target = node_approximately_at(aln_pos + delta);
            }
        }
    }
    if (same_orientation) {
        return make_pos_t(target, aln_is_rev, 0);
    } else {
        return make_pos_t(target, !aln_is_rev, 0);
    }
    /*
        && !aln_is_rev) {
    } else if (!same_direction && aln_is_rev) {
        target = (is_first_mate ? node_approximately_at(aln_pos + delta)
                  : node_approximately_at(aln_pos - delta));
    } else if (same_direction && aln_is_rev
               || !same_direction && !aln_is_rev) {
        target = (is_first_mate ? node_approximately_at(aln_pos - delta)
                  : node_approximately_at(aln_pos + delta));
    }
    */
    //bool target_is_rev = (same_orientation ? aln_is_rev : !aln_is_rev);
    //return make_pos_t(target, target_is_rev, 0);
}

bool Mapper::pair_rescue(Alignment& mate1, Alignment& mate2) {
    Timer::check();

    // bail out if we can't figure out how far to go
    if (!fragment_size) return false;
    //auto aligner = (mate1.quality().empty() ? get_regular_aligner() : get_qual_adj_aligner());
    double hang_threshold = 0.9;
    double retry_threshold = 0.7;
    //double hang_threshold = mate1.sequence().size() * aligner->match * 0.9;
    //double retry_threshold = mate1.sequence().size() * aligner->match * 0.3;
    //cerr << "hang " << hang_threshold << " retry " << retry_threshold << endl;
    //cerr << mate1.score() << " " << mate2.score() << endl;
    // based on our statistics about the alignments
    // get the subgraph overlapping the likely candidate position of the second alignment
    bool rescue_off_first = false;
    bool rescue_off_second = false;
    pos_t mate_pos;
    if (mate1.identity() > mate2.identity()
        && mate1.identity() > hang_threshold
        && mate2.identity() < retry_threshold) {
        // retry off mate1
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "Rescue read 2 off of read 1" << endl;
        }
#endif
        rescue_off_first = true;
        // record id and direction to second mate
        mate_pos = likely_mate_position(mate1, true);
    } else if (mate2.identity() > mate1.identity()
               && mate2.identity() > hang_threshold
               && mate1.identity() < retry_threshold) {
        // retry off mate2
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "Rescue read 1 off of read 2" << endl;
        }
#endif
        rescue_off_second = true;
        // record id and direction to second mate
        mate_pos = likely_mate_position(mate2, false);
    } else {
        return false;
    }
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "aiming for " << mate_pos << endl;
    }
#endif
    auto& node_cache = get_node_cache();
    auto& edge_cache = get_edge_cache();
    VG graph;
    int get_at_least = (!cached_fragment_length_mean ? fragment_max
                        : max((int)cached_fragment_length_stdev * 6 + mate1.sequence().size(),
                              mate1.sequence().size() * 4));
    cached_graph_context(graph, mate_pos, get_at_least/2, node_cache, edge_cache);
    Timer::check();
    cached_graph_context(graph, reverse(mate_pos, get_node_length(id(mate_pos))), get_at_least/2, node_cache, edge_cache);
    Timer::check();
    graph.remove_orphan_edges();
    //cerr << "got graph " << pb2json(graph.graph) << endl;
    // if we're reversed, align the reverse sequence and flip it back
    // align against it
    if (rescue_off_first) {
        Alignment aln2;
        bool flip = !mate1.path().mapping(0).position().is_reverse() && !cached_fragment_orientation
            || mate1.path().mapping(0).position().is_reverse() && cached_fragment_orientation;
        // do we expect the alignment to be on the reverse strand?
        if (flip) {
            aln2.set_sequence(reverse_complement(mate2.sequence()));
            if (!mate2.quality().empty()) {
                aln2.set_quality(mate2.quality());
                reverse(aln2.mutable_quality()->begin(),
                        aln2.mutable_quality()->end());
            }
        } else {
            aln2.set_sequence(mate2.sequence());
            if (!mate2.quality().empty()) {
                aln2.set_quality(mate2.quality());
            }
        }
        bool banded_global = false;
        bool pinned_alignment = false;
        bool pinned_reverse = false;
        aln2 = align_to_graph(aln2,
                              graph,
                              max_query_graph_ratio,
                              pinned_alignment,
                              pinned_reverse,
                              full_length_alignment_bonus,
                              banded_global);
        aln2.set_score(score_alignment(aln2));
        if (flip) {
            aln2 = reverse_complement_alignment(
                aln2,
                (function<int64_t(int64_t)>) ([&](int64_t id) {
                        return (int64_t)graph.get_node(id)->sequence().size();
                    }));
        }
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "aln2 score/ident vs " << aln2.score() << "/" << aln2.identity()
                            << " vs " << mate2.score() << "/" << mate2.identity() << endl;
        }
#endif
        if (aln2.score() > mate2.score()) {
            mate2 = aln2;
        } else {
            return false;
        }
    } else if (rescue_off_second) {
        Alignment aln1;
        bool flip = !mate2.path().mapping(0).position().is_reverse() && !cached_fragment_orientation
            || mate2.path().mapping(0).position().is_reverse() && cached_fragment_orientation;
        if (flip) {
            aln1.set_sequence(reverse_complement(mate1.sequence()));
            if (!mate1.quality().empty()) {
                aln1.set_quality(mate1.quality());
                reverse(aln1.mutable_quality()->begin(),
                        aln1.mutable_quality()->end());
            }
        } else {
            aln1.set_sequence(mate1.sequence());
            if (!mate1.quality().empty()) {
                aln1.set_quality(mate1.quality());
            }
        }
        bool banded_global = false;
        bool pinned_alignment = false;
        bool pinned_reverse = false;
        aln1 = align_to_graph(aln1,
                              graph,
                              max_query_graph_ratio,
                              pinned_alignment,
                              pinned_reverse,
                              full_length_alignment_bonus,
                              banded_global);
        //cerr << "score was " << aln1.score() << endl;
        aln1.set_score(score_alignment(aln1));
        //cerr << "score became " << aln1.score() << endl;
        if (flip) {
            aln1 = reverse_complement_alignment(
                aln1,
                (function<int64_t(int64_t)>) ([&](int64_t id) {
                        return (int64_t)graph.get_node(id)->sequence().size();
                    }));
        }
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "aln1 score/ident vs " << aln1.score() << "/" << aln1.identity()
                            << " vs " << mate1.score() << "/" << mate1.identity() << endl;
        }
#endif
        if (aln1.score() > mate1.score()) {
            mate1 = aln1;
        } else {
            return false;
        }
    }
    // if the new alignment is better
    // set the old alignment to it
    return true;
}

bool Mapper::alignments_consistent(const map<string, double>& pos1,
                                   const map<string, double>& pos2,
                                   int fragment_size_bound) {
    set<string> comm_refs;
    for (auto& p : pos1) {
        auto& name = p.first;
        if (pos2.find(name) != pos2.end()) {
            comm_refs.insert(name);
        }
    }
    // Alignments are consistent if their median node id positions are within the fragment_size
    
    // get median mapping positions
    for (auto& ref : comm_refs) {
        // this is unsafe looking, but we know they have the same keys for these values
        auto mean1 = pos1.find(ref)->second;
        auto mean2 = pos2.find(ref)->second;
        if (abs(mean1 - mean2) < fragment_size_bound) {
            return true;
        }
    }
    return false;
}

bool Mapper::pair_consistent(const Alignment& aln1,
                             const Alignment& aln2) {
    if (!(aln1.score() && aln2.score())) return false;
    bool length_ok = false;
    if (aln1.fragment_size() == 0) {
        // use the approximate distance
        int len = approx_fragment_length(aln1, aln2);
        if (len > 0 && len < fragment_size
            || !fragment_size && len > 0 && len < fragment_max) {
            length_ok = true;
        }
    } else {
        // use the distance induced by the graph paths
        assert(aln1.fragment_size() == aln2.fragment_size());
        for (size_t i = 0; i < aln1.fragment_size(); ++i) {
            int len = abs(aln1.fragment(i).length());
            if (len > 0 && len < fragment_size
                || !fragment_size && len > 0 && len < fragment_max) {
                length_ok = true;
                break;
            }
        }
    }
    bool aln1_is_rev = aln1.path().mapping(0).position().is_reverse();
    bool aln2_is_rev = aln1.path().mapping(0).position().is_reverse();
    bool same_orientation = cached_fragment_orientation;
    bool orientation_ok = same_orientation && aln1_is_rev == aln2_is_rev
        || !same_orientation && aln1_is_rev != aln2_is_rev;
    return length_ok && orientation_ok;
}

pair<vector<Alignment>, vector<Alignment>> Mapper::align_paired_multi(
    const Alignment& read1,
    const Alignment& read2,
    bool& queued_resolve_later,
    int kmer_size,
    int stride,
    int max_mem_length,
    int band_width,
    int pair_window,
    bool only_top_scoring_pair,
    bool retrying) {

    // use mem threading if requested and we have not need to band (not implemented)
    if (mem_chaining && read1.sequence().size() < band_width) {
        if (simultaneous_pair_alignment) {
            return align_paired_multi_simul(read1,
                                            read2,
                                            queued_resolve_later,
                                            max_mem_length,
                                            only_top_scoring_pair,
                                            retrying);
        } else {
            return align_paired_multi_combi(read1,
                                            read2,
                                            queued_resolve_later,
                                            kmer_size,
                                            stride,
                                            max_mem_length,
                                            band_width,
                                            only_top_scoring_pair,
                                            retrying);
        }
    } else {
        return align_paired_multi_sep(read1,
                                      read2,
                                      queued_resolve_later,
                                      kmer_size,
                                      stride,
                                      max_mem_length,
                                      band_width,
                                      pair_window,
                                      only_top_scoring_pair,
                                      retrying);
    }
}

pair<vector<Alignment>, vector<Alignment>> Mapper::align_paired_multi_sep(
    const Alignment& read1,
    const Alignment& read2,
    bool& queued_resolve_later,
    int kmer_size,
    int stride,
    int max_mem_length,
    int band_width,
    int pair_window,
    bool only_top_scoring_pair,
    bool retrying) {
    // We have some logic around align_mate_in_window to handle orientation
    // Since we now support reversing edges, we have to at least try opposing orientations for the reads.
    auto align_mate = [&](const Alignment& read, Alignment& mate) {
        // Make an alignment to align in the same local orientation as the read
        Alignment aln_same = mate;
        aln_same.clear_path();
        // And one to align in the opposite local orientation
        // Always reverse the opposite direction sequence
        Alignment aln_opposite = reverse_complement_alignment(aln_same, [&](id_t id) {return get_node_length(id);});

        // We can't rescue off an unmapped read
        assert(read.has_path() && read.path().mapping_size() > 0);

        // Do both the alignments
        align_mate_in_window(read, aln_same, pair_window);
        align_mate_in_window(read, aln_opposite, pair_window);

        if(aln_same.score() >= aln_opposite.score()) {
            // TODO: we should prefer opposign local orientations, but we can't
            // really measure them well.
            mate = aln_same;
        } else {
            // Flip the winning reverse alignment back to the original read orientation
            aln_opposite = reverse_complement_alignment(aln_opposite, [&](id_t id) {
                    return get_node_length(id);
                });
            mate = aln_opposite;
        }
    };

    // find the MEMs for the alignments
    vector<MaximalExactMatch> mems1 = find_mems_deep(read1.sequence().begin(),
                                                     read1.sequence().end(),
                                                     max_mem_length,
                                                     min_mem_length,
                                                     mem_reseed_length);
    vector<MaximalExactMatch> mems2 = find_mems_deep(read2.sequence().begin(),
                                                     read2.sequence().end(),
                                                     max_mem_length,
                                                     min_mem_length,
                                                     mem_reseed_length);
    //cerr << "mems before " << mems1.size() << " " << mems2.size() << endl;
    // Do the initial alignments, making sure to get some extras if we're going to check consistency.

    vector<MaximalExactMatch> pairable_mems1, pairable_mems2;
    vector<MaximalExactMatch>* pairable_mems_ptr_1 = nullptr;
    vector<MaximalExactMatch>* pairable_mems_ptr_2 = nullptr;

    // sensitivity ramp
    // first try to get a consistent pair
    // if none is found, re-run the MEM generation with a shorter MEM length
    // if still none is found, align independently with full MEMS; report inconsistent pair
    // optionally use local resolution ...

    // wishlist
    // break out the entire MEM determination logic
    // and merge it with the clustering
    //

    // find the MEMs for the alignments
    if (fragment_size) {
        // use pair resolution filterings on the SMEMs to constrain the candidates
        set<MaximalExactMatch*> pairable_mems = resolve_paired_mems(mems1, mems2);
        for (auto& mem : mems1) if (pairable_mems.count(&mem)) pairable_mems1.push_back(mem);
        for (auto& mem : mems2) if (pairable_mems.count(&mem)) pairable_mems2.push_back(mem);
        pairable_mems_ptr_1 = &pairable_mems1;
        pairable_mems_ptr_2 = &pairable_mems2;
    } else {
        pairable_mems_ptr_1 = &mems1;
        pairable_mems_ptr_2 = &mems2;
    }

    //cerr << pairable_mems1.size() << " and " << pairable_mems2.size() << endl;

    bool report_consistent_pairs = (bool) fragment_size;

    // use MEM alignment on the MEMs matching our constraints
    // We maintain the invariant that these two vectors of alignments are sorted
    // by score, descending, as returned from align_multi_internal.
    double cluster_mq1, cluster_mq2; // XXX not enabled
    vector<Alignment> alignments1 = align_multi_internal(false, read1, kmer_size, stride, max_mem_length,
                                                         band_width, cluster_mq1, extra_multimaps, pairable_mems_ptr_1);
    vector<Alignment> alignments2 = align_multi_internal(false, read2, kmer_size, stride, max_mem_length,
                                                         band_width, cluster_mq2, extra_multimaps, pairable_mems_ptr_2);

    size_t best_score1 = 0;
    size_t best_score2 = 0;
    // A nonzero best score means we have any valid alignments of that read.
    for (auto& aln : alignments1) best_score1 = max(best_score1, (size_t)aln.score());
    for (auto& aln : alignments2) best_score2 = max(best_score2, (size_t)aln.score());

    //bool rescue = !mem_chaining && fragment_size != 0; // don't try to rescue if we have a defined fragment size
    bool rescue = fragment_size != 0;

    // Rescue only if the top alignment on one side has no mappings
    if(rescue && best_score1 == 0 && best_score2 != 0) {
        // Must rescue 1 off of 2
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "Rescue read 1 off of read 2" << endl;
        }
#endif
        alignments1.clear();

        // We use this to deduplicate rescue alignments based on their
        // serialized Prtotobuf paths. Relies on protobuf serialization being
        // deterministic.
        set<string> found;

        for(auto base : alignments2) {
            if(base.score() == 0 || !base.has_path() || base.path().mapping_size() == 0) {
                // Can't rescue off this
                continue;
            }
            Alignment mate = read1;
            align_mate(base, mate);

            string serialized;
            mate.path().SerializeToString(&serialized);
            if(!found.count(serialized)) {
                // This is a novel alignment
                alignments1.push_back(mate);
                found.insert(serialized);
            }

            if(!always_rescue) {
                // We only want to rescue off the best one, and they're sorted
                break;
            }
        }
    } else if(rescue && best_score1 != 0 && best_score2 == 0) {
        // Must rescue 2 off of 1
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "Rescue read 2 off of read 1" << endl;
        }
#endif
        alignments2.clear();

        // We use this to deduplicate rescue alignments based on their
        // serialized Prtotobuf paths. Relies on protobuf serialization being
        // deterministic.
        set<string> found;

        for(auto base : alignments1) {
            if(base.score() == 0 || !base.has_path() || base.path().mapping_size() == 0) {
                // Can't rescue off this
                continue;
            }
            Alignment mate = read2;
            align_mate(base, mate);

            string serialized;
            mate.path().SerializeToString(&serialized);
            if(!found.count(serialized)) {
                // This is a novel alignment
                alignments2.push_back(mate);
                found.insert(serialized);
            }

            if(!always_rescue) {
                // We only want to rescue off the best one, and they're sorted
                break;
            }
        }
    } else if(always_rescue) {
        // Try rescuing each off all of the other.
        // We need to be concerned about introducing duplicates.

        // We need temp places to hold the extra alignments we make so as to not
        // rescue off rescues.
        vector<Alignment> extra1;
        vector<Alignment> extra2;

        // We use these to deduplicate alignments based on their serialized
        // Prtotobuf paths. Relies of protobuf serialization being
        // deterministic.
        set<string> found1;
        set<string> found2;

        // Fill in the known alignments
        for(auto existing : alignments1) {
            // Serialize each alignment's Path and put the result in the set
            string serialized;
            existing.path().SerializeToString(&serialized);
            found1.insert(serialized);
        }
        for(auto existing : alignments2) {
            // Serialize each alignment's Path and put the result in the set
            string serialized;
            existing.path().SerializeToString(&serialized);
            found2.insert(serialized);
        }

        for(auto base : alignments1) {
            // Do 2 off of 1
            if(base.score() == 0 || !base.has_path() || base.path().mapping_size() == 0) {
                // Can't rescue off this
                continue;
            }
            Alignment mate = read2;
            align_mate(base, mate);

            string serialized;
            mate.path().SerializeToString(&serialized);
            if(!found2.count(serialized)) {
                // This is a novel alignment
                extra2.push_back(mate);
                found2.insert(serialized);
            }
        }

        for(auto base : alignments2) {
            // Do 1 off of 2
            if(base.score() == 0 || !base.has_path() || base.path().mapping_size() == 0) {
                // Can't rescue off this
                continue;
            }
            Alignment mate = read1;
            align_mate(base, mate);

            string serialized;
            mate.path().SerializeToString(&serialized);
            if(!found1.count(serialized)) {
                // This is a novel alignment
                extra1.push_back(mate);
                found1.insert(serialized);
            }
        }

        // Copy over the new unique alignments
        alignments1.insert(alignments1.end(), extra1.begin(), extra1.end());
        alignments2.insert(alignments2.end(), extra2.begin(), extra2.end());
    }

    // Fix up the sorting by score, descending, in case rescues came out
    // better than normal alignments.
    sort(alignments1.begin(), alignments1.end(), [](const Alignment& a, const Alignment& b) {
            return a.score() > b.score();
        });
    sort(alignments2.begin(), alignments2.end(), [](const Alignment& a, const Alignment& b) {
            return a.score() > b.score();
        });

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << alignments1.size() << " alignments for read 1, " << alignments2.size() << " for read 2" << endl;
    }
#endif

    pair<vector<Alignment>, vector<Alignment>> results;

    bool found_consistent = false;


    if (fragment_size) {

        map<Alignment*, map<string, double> > aln_pos;
        for (auto& aln : alignments1) {
            aln_pos[&aln] = alignment_mean_path_positions(aln);
        }
        for (auto& aln : alignments2) {
            aln_pos[&aln] = alignment_mean_path_positions(aln);
        }

        // Now we want to emit consistent pairs, in order of decreasing total score.

        // compare pairs by the sum of their individual scores
        // We need this functor thing to make the priority queue work.
        struct ComparePairedAlignmentScores {
            vector<Alignment>& alns_1;
            vector<Alignment>& alns_2;

        public:
            ComparePairedAlignmentScores(vector<Alignment>& alns_1, vector<Alignment>& alns_2) : alns_1(alns_1), alns_2(alns_2) {}
            bool operator()(const pair<int, int> idxs1, const pair<int, int> idxs2) {
                return (alns_1[idxs1.first].score() + alns_2[idxs1.second].score()
                        < alns_1[idxs2.first].score() + alns_2[idxs2.second].score());
            }
        };

        ComparePairedAlignmentScores compare_paired_alignment_scores = ComparePairedAlignmentScores(alignments1, alignments2);

        // think about the pairs being laid out on a grid over the individual end multimaps, sorted in each dimension by score
        // navigate from top left corner outward to add consistent pairs in decreasing score order
        priority_queue<pair<int, int>, vector<pair<int, int>>, ComparePairedAlignmentScores> pair_queue(compare_paired_alignment_scores);
        // keep track of which indices have been checked to avoid checking them twice when navigating from above and from the left
        std::unordered_set<pair<int, int>> considered_pairs;

        pair<vector<Alignment>, vector<Alignment>> consistent_pairs;
        // ensure that there is always an additional pair to compute a mapping quality against
        int num_pairs = max_multimaps >= 2 ? max_multimaps : 2;

        pair_queue.push(make_pair(0, 0));
        while (!pair_queue.empty() && consistent_pairs.first.size() < num_pairs) {
            // get index of remaining pair with highest combined score
            pair<int, int> aln_pair = pair_queue.top();
            pair_queue.pop();


            if (alignments_consistent(aln_pos[&alignments1[aln_pair.first]], aln_pos[&alignments2[aln_pair.second]], fragment_size)) {
                found_consistent = true;
                consistent_pairs.first.push_back(alignments1[aln_pair.first]);
                consistent_pairs.second.push_back(alignments2[aln_pair.second]);

                if(debug) {
                    cerr << "Found consistent pair " << aln_pair.first << ", " << aln_pair.second
                         << " with scores " << alignments1[aln_pair.first].score()
                         << ", " << alignments2[aln_pair.second].score() << endl;
                }

            }

            // add in the two adjacent indices if we haven't already
            pair<int,int> next_aln_pair_down = make_pair(aln_pair.first + 1, aln_pair.second);
            pair<int,int> next_aln_pair_right = make_pair(aln_pair.first, aln_pair.second + 1);
            if (next_aln_pair_down.first < alignments1.size() && considered_pairs.find(next_aln_pair_down) == considered_pairs.end()) {
                pair_queue.push(next_aln_pair_down);
                considered_pairs.insert(next_aln_pair_down);
            }
            if (next_aln_pair_right.second < alignments2.size() && considered_pairs.find(next_aln_pair_right) == considered_pairs.end()) {
                pair_queue.push(next_aln_pair_right);
                considered_pairs.insert(next_aln_pair_right);
            }
        }

        compute_mapping_qualities(consistent_pairs, cluster_mq1+cluster_mq2);

        // remove the extra pair used to compute mapping quality if necessary
        if (consistent_pairs.first.size() > max_multimaps) {
            consistent_pairs.first.resize(max_multimaps);
            consistent_pairs.second.resize(max_multimaps);
        }

        // mark primary and secondary
        for (int i = 0; i < consistent_pairs.first.size(); i++) {
            consistent_pairs.first[i].mutable_fragment_next()->set_name(read2.name());
            consistent_pairs.first[i].set_is_secondary(i > 0);
            consistent_pairs.second[i].mutable_fragment_prev()->set_name(read1.name());
            consistent_pairs.second[i].set_is_secondary(i > 0);
        }

        // zap everything unless the primary alignments are individually top-scoring
        if (only_top_scoring_pair && consistent_pairs.first.size() &&
            (consistent_pairs.first[0].score() < alignments1[0].score() ||
             consistent_pairs.second[0].score() < alignments2[0].score())) {
            consistent_pairs.first.clear();
            consistent_pairs.second.clear();
        }

        if (!consistent_pairs.first.empty()) {
            results = consistent_pairs;
        } else {
            // no consistent pairs found
            // if we can decrease our MEM size
            // clear, to trigger size reduction

            // otherwise, yolo
        }

    } else {

        results = make_pair(alignments1, alignments2);
        compute_mapping_qualities(results, cluster_mq1 + cluster_mq2);

        // Truncate to max multimaps
        if(results.first.size() > max_multimaps) {
            results.first.resize(max_multimaps);
        }
        if(results.second.size() > max_multimaps) {
            results.second.resize(max_multimaps);
        }

        // mark primary and secondary
        for (int i = 0; i < results.first.size(); i++) {
            results.first[i].mutable_fragment_next()->set_name(read2.name());
            results.first[i].set_is_secondary(i > 0);
        }
        for (int i = 0; i < results.second.size(); i++) {
            results.second[i].mutable_fragment_prev()->set_name(read1.name());
            results.second[i].set_is_secondary(i > 0);
        }

    }

    // change the potential set of MEMs by dropping the maximum MEM size
    // this tends to slightly boost sensitivity at minimal cost
    if (results.first.empty()
        || results.second.empty()
        || !results.first.front().score()
        || !results.second.front().score()) {
        //|| fragment_size && !found_consistent) {
        //cerr << "failed alignment" << endl;
        if (kmer_sensitivity_step) {
            int new_mem_max = max((int) min_mem_length,
                                  (int) (max_mem_length ? max_mem_length : gcsa->order()) - kmer_sensitivity_step);
            if (new_mem_max == min_mem_length) {
                // do noting
            } else if (new_mem_max > min_mem_length) {
                //cerr << "trying with " << new_mem_max << endl;
                return align_paired_multi_sep(read1, read2,
                                              queued_resolve_later,
                                              kmer_size, stride,
                                              new_mem_max,
                                              band_width, pair_window);
            }
        }
    }

    // we tried to align
    // if we don't have a fragment_size yet determined
    // and we didn't get a perfect, unambiguous hit on both reads
    // we'll need to try it again later when we do have a fragment_size
    // so store it in a buffer local to this mapper

    // tag the results with their fragment lengths
    // record the lengths in a deque that we use to keep a running estimate of the fragment length distribution
    // we then set the fragment_size cutoff using the moments of the estimated distribution
    bool imperfect_pair = false;
    for (int i = 0; i < min(results.first.size(), results.second.size()); ++i) {
        if (retrying) break;
        auto& aln1 = results.first.at(i);
        auto& aln2 = results.second.at(i);
        auto approx_frag_lengths = approx_pair_fragment_length(aln1, aln2);
        for (auto& j : approx_frag_lengths) {
            // record the fragment length information in the alignment record
            Path fragment;
            fragment.set_name(j.first);
            fragment.set_length(j.second);
            *aln1.add_fragment() = fragment;
            *aln2.add_fragment() = fragment;
            if (results.first.size() == 1
                && results.second.size() == 1
                && results.first.front().identity() > perfect_pair_identity_threshold
                && results.second.front().identity() > perfect_pair_identity_threshold
                && (fragment_size && j.second < fragment_size
                    || !fragment_size && j.second < fragment_max)) { // hard cutoff
                //cerr << "aln\tperfect alignments" << endl;
                record_fragment_configuration(j.second, aln1, aln2);
            } else if (!fragment_size) {
                imperfect_pair = true;
            }
            //cerr << "aln\t" << aln1.name() << "\t" << aln2.name() << "\t" << j.first << "\t" << j.second << "\t"
            //     << cached_fragment_length_mean << "\t" << cached_fragment_length_stdev << endl;
            //<< fragment_length_mean() << "\t" << fragment_length_stdev() << "\t"
        }
    }

    if (!retrying && imperfect_pair && fragment_max) {
        imperfect_pairs_to_retry.push_back(make_pair(read1, read2));
        results.first.clear();
        results.second.clear();
        // we signal the fact that this isn't a perfect pair, so we don't write it out externally?
        queued_resolve_later = true;
    }

    // remove results we don't need given our requested number of multimaps
    if (results.first.size() > max_multimaps) {
        results.first.resize(max_multimaps);
    }
    if (results.second.size() > max_multimaps) {
        results.second.resize(max_multimaps);
    }

    if(results.first.empty()) {
        results.first.push_back(read1);
        auto& aln = results.first.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }
    if(results.second.empty()) {
        results.second.push_back(read2);
        auto& aln = results.second.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }

    // Make sure to link up alignments even if they aren't mapped.
    for (auto& aln : results.first) {
        aln.mutable_fragment_next()->set_name(read2.name());
    }

    for (auto& aln : results.second) {
        aln.mutable_fragment_prev()->set_name(read1.name());
    }

    return results;
    
}

/// cross all single-ended alignments from each read
/// then sort by joint score scaled by a pair bonus, which is computed from the max fragment and observed size distribution
pair<vector<Alignment>, vector<Alignment>> Mapper::align_paired_multi_combi(
    const Alignment& read1,
    const Alignment& read2,
    bool& queued_resolve_later,
    int kmer_size,
    int stride,
    int max_mem_length,
    int band_width,
    bool only_top_scoring_pair,
    bool retrying) {

    //double avg_node_len = average_node_length();
    int8_t match;
    int8_t gap_extension;
    int8_t gap_open;
    if (read1.quality().empty() || !adjust_alignments_for_base_quality) {
        auto aligner =  get_regular_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    else {
        auto aligner =  get_qual_adj_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    int total_multimaps = max_multimaps + extra_multimaps;

    // use MEM alignment on the MEMs matching our constraints
    // We maintain the invariant that these two vectors of alignments are sorted
    // by score, descending, as returned from align_multi_internal.
    double cluster_mq1, cluster_mq2;
    vector<Alignment> alignments1 = align_multi_internal(false, read1, kmer_size, stride, max_mem_length,
                                                         band_width, cluster_mq1, extra_multimaps, nullptr);
    vector<Alignment> alignments2 = align_multi_internal(false, read2, kmer_size, stride, max_mem_length,
                                                         band_width, cluster_mq2, extra_multimaps, nullptr);

    size_t best_score1 = 0;
    size_t best_score2 = 0;
    // A nonzero best score means we have any valid alignments of that read.
    for (auto& aln : alignments1) best_score1 = max(best_score1, (size_t)aln.score());
    for (auto& aln : alignments2) best_score2 = max(best_score2, (size_t)aln.score());

    // consider all crosses of the alignments where the distance between them is less than the max
    // then sort these by the score of the whole pair
    // and de-dup
    //
    // zip the alns up into possible pairs
    // for each pair... estimate distance
    // make a pair for each
    // score them by how well they match the alignment distribution
    // add in all the singular alignments too
    //
    Alignment unaligned1=read1, unaligned2=read2;
    unaligned1.clear_path();
    unaligned1.clear_score();
    unaligned2.clear_path();
    unaligned2.clear_score();
    typedef struct {
        Alignment* mate1;
        Alignment* mate2;
        int score;
        double bonus;
    } AlignmentPair;
    vector<AlignmentPair> alnpairs;
    for (auto& aln1 : alignments1) {
        for (auto& aln2 : alignments2) {
            if (&aln1 != &aln2) {
                // add this combination
                alnpairs.emplace_back();
                alnpairs.back().mate1 = &aln1;
                alnpairs.back().mate2 = &aln2;
                // add a pair for each with the mate unaligned
                alnpairs.emplace_back();
                alnpairs.back().mate1 = &unaligned1;
                alnpairs.back().mate2 = &aln2;
            }
        }
        // add a pair for each with the mate unaligned
        alnpairs.emplace_back();
        alnpairs.back().mate1 = &aln1;
        alnpairs.back().mate2 = &unaligned2;
    }
    vector<AlignmentPair*> alns;
    for (auto& p : alnpairs) alns.push_back(&p);

    auto score_sort_and_dedup = [&](void) {
        // add a bonus score for alignments that are within bounds
        for (auto& alnpair : alnpairs) {
            alnpair.score = alnpair.mate1->score() + alnpair.mate2->score();
            // see if we should compute a pair matching bonus
            if (alnpair.score) {
                int dist = approx_fragment_length(*alnpair.mate1, *alnpair.mate2);
                if (fragment_size) {
                    if (pair_consistent(*alnpair.mate1, *alnpair.mate2)) {
                        alnpair.bonus = alnpair.score * fragment_length_pdf(dist)/fragment_length_pdf(cached_fragment_length_mean);
                    }
                } else {
                    if (dist > 0) {
                        if (dist < fragment_max) {
                            alnpair.bonus = alnpair.score;
                        }
                    }
                }
            }
        }
        // sort the aligned pairs by bonus, or score if neither has a bonus
        std::sort(alns.begin(), alns.end(),
                  [&](const AlignmentPair* pair1,
                      const AlignmentPair* pair2) {
                      if (pair1->bonus || pair2->bonus) {
                          return pair1->bonus > pair2->bonus;
                      } else {
                          return pair1->score > pair2->score;
                      }
                  });
        // remove duplicates (same score and same start position of both pairs)
        alns.erase(
            std::unique(
                alns.begin(), alns.end(),
                [&](const AlignmentPair* pair1,
                    const AlignmentPair* pair2) {
                    bool same = true;
                    if (pair1->mate1->score() && pair2->mate1->score()) {
                        same &= make_pos_t(pair1->mate1->path().mapping(0).position())
                            == make_pos_t(pair2->mate1->path().mapping(0).position());
                    }
                    if (pair1->mate2->score() && pair2->mate2->score()) {
                        same &= make_pos_t(pair1->mate2->path().mapping(0).position())
                            == make_pos_t(pair2->mate2->path().mapping(0).position());
                    }
                    if (!(pair1->mate1->score() && pair2->mate1->score()
                          || pair1->mate2->score() && pair2->mate2->score())) {
                        same = false;
                    }
                    return same;
                }),
            alns.end());
        if (alns.size() > total_multimaps) {
            alns.erase(alns.begin()+total_multimaps, alns.end());
        }
    };

    score_sort_and_dedup();
#pragma omp critical
    if (debug) {
        cerr << "alignment pairs" << endl;
        for (auto& p : alns) {
            cerr << p->bonus << " " << p->mate1->score() << " " << p->mate2->score() << " ";
            if (p->mate1->score()) cerr << " pos1 " << p->mate1->path().mapping(0).position().node_id() << " ";
            if (p->mate2->score()) cerr << " pos2 " << p->mate2->path().mapping(0).position().node_id() << " ";
            if (pair_consistent(*p->mate1, *p->mate2)) cerr << "consistent";
            cerr << endl;
        }
    }
    // don't rescue; TODO test enabling this
    /*
    if (fragment_size) {
        // go through the pairs and see if we need to rescue one side off the other
        bool rescued = false;
        for (auto& p : alns) {
            rescued |= pair_rescue(*p->mate1, *p->mate2);
        }
        // if we rescued, resort and remove dups
        if (rescued) {
            score_sort_and_dedup();
        }
    }
    */

    pair<vector<Alignment>, vector<Alignment>> results;

    // rebuild the thing we'll return
    int read1_max_score = 0;
    int read2_max_score = 0;
    for (auto& p : alns) {
        read1_max_score = max(p->mate1->score(), read1_max_score);
        read2_max_score = max(p->mate2->score(), read2_max_score);
        results.first.push_back(*p->mate1);
        results.second.push_back(*p->mate2);
    }

    // compute mapping qualities
    if (!results.first.empty()) {
        compute_mapping_qualities(results, max(cluster_mq1, cluster_mq2));
        //compute_mapping_qualities(results, max(cluster_mq1, cluster_mq2));
        /*
        compute_mapping_qualities(results.first, cluster_mq1);
        compute_mapping_qualities(results.second, cluster_mq2);
        */
        // do we meet the fragment size requirements
        /*
        auto& mate1 = results.first.front();
        auto& mate2 = results.second.front();
        // if not, we downgrade the mapping quality in an ad-hoc way
        // TODO could we do this in a way that reflects this pair's specific fragment length?
        if (pair_consistent(mate1, mate2)) {
            // if the pair is consistent, compute the joint mapping quality
            compute_mapping_qualities(results, max(cluster_mq1, cluster_mq2));
        } else {
            compute_mapping_qualities(results, min(cluster_mq1, cluster_mq2));
            //compute_mapping_qualities(results.first, cluster_mq1);
            //compute_mapping_qualities(results.second, cluster_mq2);
        }
        */
    }

    // remove the extra pair used to compute mapping quality if necessary
    if (results.first.size() > max_multimaps) {
        results.first.resize(max_multimaps);
        results.second.resize(max_multimaps);
    }

    // mark primary and secondary
    for (int i = 0; i < results.first.size(); i++) {
        results.first[i].mutable_fragment_next()->set_name(read2.name());
        results.first[i].set_is_secondary(i > 0);
        results.second[i].mutable_fragment_prev()->set_name(read1.name());
        results.second[i].set_is_secondary(i > 0);
    }

    // optionally zap everything unless the primary alignments are individually top-scoring
    if (only_top_scoring_pair && results.first.size() &&
        (results.first[0].score() < read1_max_score ||
         results.second[0].score() < read2_max_score)) {
        results.first.clear();
        results.second.clear();
    }

    // we tried to align
    // if we don't have a fragment_size yet determined
    // and we didn't get a perfect, unambiguous hit on both reads
    // we'll need to try it again later when we do have a fragment_size
    // so store it in a buffer local to this mapper

    // tag the results with their fragment lengths
    // record the lengths in a deque that we use to keep a running estimate of the fragment length distribution
    // we then set the fragment_size cutoff using the moments of the estimated distribution
    bool imperfect_pair = false;
    for (int i = 0; i < min(results.first.size(), results.second.size()); ++i) {
        if (retrying) break;
        auto& aln1 = results.first.at(i);
        auto& aln2 = results.second.at(i);
        auto approx_frag_lengths = approx_pair_fragment_length(aln1, aln2);
        for (auto& j : approx_frag_lengths) {
            // record the fragment length information in the alignment record
            Path fragment;
            fragment.set_name(j.first);
            fragment.set_length(j.second);
            *aln1.add_fragment() = fragment;
            *aln2.add_fragment() = fragment;
            // if we have a perfect mapping, and we're under our hard fragment length cutoff
            // push the result into our deque of fragment lengths
            if (results.first.size() == 1
                && results.second.size() == 1
                && results.first.front().identity() > perfect_pair_identity_threshold
                && results.second.front().identity() > perfect_pair_identity_threshold
                && (fragment_size && abs(j.second) < fragment_size
                    || !fragment_size && abs(j.second) < fragment_max)) { // hard cutoff
                //cerr << "aln\tperfect alignments" << endl;
                record_fragment_configuration(j.second, aln1, aln2);
            } else if (!fragment_size) {
                imperfect_pair = true;
            }
        }
    }

    if (!retrying && imperfect_pair && fragment_max) {
        imperfect_pairs_to_retry.push_back(make_pair(read1, read2));
        results.first.clear();
        results.second.clear();
        // we signal the fact that this isn't a perfect pair, so we don't write it out externally?
        queued_resolve_later = true;
    }

    // do not compute the paired mapping quality
    // this does not seem to be doing the right thing in this context

    if(results.first.empty()) {
        results.first.push_back(read1);
        auto& aln = results.first.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }
    if(results.second.empty()) {
        results.second.push_back(read2);
        auto& aln = results.second.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }

    // Make sure to link up alignments even if they aren't mapped.
    for (auto& aln : results.first) {
        aln.set_name(read1.name());
        aln.mutable_fragment_next()->set_name(read2.name());
    }

    for (auto& aln : results.second) {
        aln.set_name(read2.name());
        aln.mutable_fragment_prev()->set_name(read1.name());
    }

    return results;

}


    
pair<vector<Alignment>, vector<Alignment>> Mapper::align_paired_multi_simul(
    const Alignment& read1,
    const Alignment& read2,
    bool& queued_resolve_later,
    int max_mem_length,
    bool only_top_scoring_pair,
    bool retrying) {

    double avg_node_len = average_node_length();
    int8_t match;
    int8_t gap_extension;
    int8_t gap_open;
    if (read1.quality().empty() || !adjust_alignments_for_base_quality) {
        auto aligner =  get_regular_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    else {
        auto aligner =  get_qual_adj_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    int total_multimaps = max_multimaps + extra_multimaps;
    double cluster_mq = 0;

    if(debug) {
        cerr << "align_paired_multi_simul "
             << "with " << read1.name() << " and "
             << read2.name() << endl
            //<< "read 1 " << read1.sequence() << endl
            //<< "read 2 " << read2.sequence() << endl
            //<< "read 1 " << pb2json(read1) << endl
            //<< "read 2 " << pb2json(read2) << endl
             << "fragment model " << fragment_max << ", "
             << fragment_size << ", "
             << cached_fragment_length_mean << ", "
             << cached_fragment_length_stdev << ", "
             << cached_fragment_orientation << ", "
             << cached_fragment_direction << ", "
             << since_last_fragment_length_estimate << ", " << endl;
    }

    pair<vector<Alignment>, vector<Alignment>> results;

    // find the MEMs for the alignments
    vector<MaximalExactMatch> mems1 = find_mems_deep(read1.sequence().begin(),
                                                     read1.sequence().end(),
                                                     max_mem_length,
                                                     min_mem_length,
                                                     mem_reseed_length);
//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "mems for read 1 " << mems_to_json(mems1) << endl;
    }
//#endif
    vector<MaximalExactMatch> mems2 = find_mems_deep(read2.sequence().begin(),
                                                     read2.sequence().end(),
                                                     max_mem_length,
                                                     min_mem_length,
                                                     mem_reseed_length);
//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "mems for read 2 " << mems_to_json(mems2) << endl;
    }
//#endif

    auto transition_weight = [&](const MaximalExactMatch& m1, const MaximalExactMatch& m2) {

        // set up positions for distance query
        pos_t m1_pos = make_pos_t(m1.nodes.front());
        pos_t m2_pos = make_pos_t(m2.nodes.front());
        double uniqueness = 2.0 / (m1.match_count + m2.match_count);

        // approximate distance by node lengths
        int approx_dist = approx_distance(m1_pos, m2_pos);
        bool relative_direction = is_rev(m1_pos) ? approx_dist < 0 : approx_dist > 0;

        // are the two mems in a different fragment?
        // we handle the distance metric differently in these cases
        if (m1.fragment < m2.fragment) {
            int max_length = fragment_max;
            int dist = abs(approx_dist);
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "between fragment approx distance " << approx_dist << endl;
            }
#endif
            if (dist >= max_length) {
                return -std::numeric_limits<double>::max();
            } else {
                if (xindex->path_count) {
                    dist = xindex->min_approx_path_distance({}, id(m1_pos), id(m2_pos));
                }
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "---> true distance from " << m1_pos << " to " << m2_pos << " = "<< dist << endl;
                }
#endif
                if (dist >= max_length) {
                    return -std::numeric_limits<double>::max();
                } else if (fragment_size) {
                    // exclude cases that don't match our model
                    if (!cached_fragment_orientation
                        && is_rev(m1_pos) == is_rev(m2_pos)
                        || cached_fragment_orientation
                        && is_rev(m1_pos) != is_rev(m2_pos)
                        || dist > fragment_size) {
                        return -std::numeric_limits<double>::max();
                    } else {
                        return fragment_length_pdf(dist)/fragment_length_pdf(cached_fragment_length_mean);
                    }
                } else {
                    return 1.0/dist;
                }
            }
        } else if (m1.fragment > m2.fragment) {
            // don't allow going backwards in the threads
            return -std::numeric_limits<double>::max();
        } else {
            int max_length = 2 * (m1.length() + m2.length());
            // find the difference in m1.end and m2.begin
            // find the positional difference in the graph between m1.end and m2.begin
            int unique_coverage = m1.length() + m2.length() - mems_overlap_length(m1, m2);
            approx_dist = abs(approx_dist);
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "in fragment approx distance " << approx_dist << endl;
            }
#endif
            if (approx_dist > max_length) {
                // too far
                return -std::numeric_limits<double>::max();
            } else {
                // we may want to switch back to exact measurement, although the approximate metric is simpler and more reliable despite being less precise
                // int distance = graph_distance(m1_pos, m2_pos, max_length); // enable for exact distance calculation
                int distance = approx_dist;
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "---> true distance " << distance << endl;
                }
#endif
                if (distance == max_length) {
                    return -std::numeric_limits<double>::max();
                }
                if (is_rev(m1_pos) != is_rev(m2_pos)) {
                    // disable inversions
                    return -std::numeric_limits<double>::max();
                } else {
                    // accepted transition
                    double jump = abs((m2.begin - m1.begin) - distance);
                    if (jump) {
                        return (double) unique_coverage * match * uniqueness - (gap_open + jump * gap_extension);
                    } else {
                        return (double) unique_coverage * match * uniqueness;
                    }
                }
            }
        }
    };

    Timer::check();

    // build the paired-read MEM markov model
    MEMChainModel markov_model({ read1.sequence().size(), read2.sequence().size() }, { mems1, mems2 }, this, transition_weight, max((int)(read1.sequence().size() + read2.sequence().size()), (int)(fragment_size ? fragment_size : fragment_max)));
    vector<vector<MaximalExactMatch> > clusters = markov_model.traceback(total_multimaps, true, debug);

    // don't attempt to align if we reach the maximum number of multimaps
    //if (clusters.size() == total_multimaps) clusters.clear();
    // disabled as this seems to cause serious problems at low cluster sizes

    auto show_clusters = [&](void) {
        cerr << "clusters: " << endl;
        for (auto& cluster : clusters) {
            cerr << cluster.size() << " MEMs covering " << cluster_coverage(cluster) << " @ ";
            for (auto& mem : cluster) {
                size_t len = mem.begin - mem.end;
                for (auto& node : mem.nodes) {
                    id_t id = gcsa::Node::id(node);
                    size_t offset = gcsa::Node::offset(node);
                    bool is_rev = gcsa::Node::rc(node);
                    cerr << "|" << id << (is_rev ? "-" : "+") << ":" << offset << "," << mem.fragment << ",";
                }
                cerr << mem.sequence() << " ";
            }
            cerr << endl;
        }
    };

#pragma omp critical
    {
        if (debug) {
            cerr << "### clusters:" << endl;
            show_clusters();
        }
    }

    vector<vector<MaximalExactMatch> > clusters1;
    vector<vector<MaximalExactMatch> > clusters2;
    for (auto& cluster : clusters) {
        clusters1.emplace_back();
        clusters2.emplace_back();
        auto& cluster1 = clusters1.back();
        auto& cluster2 = clusters2.back();
        // break the cluster into two pieces
        bool seen1=false, seen2=false;
        for (auto& mem : cluster) {
            if (!seen2 && mem.fragment == 1) {
                cluster1.push_back(mem);
                seen1 = true;
            } else if (mem.fragment == 2) {
                cluster2.push_back(mem);
                seen2 = true;
            } else {
                cerr << "vg map error misordered fragments in cluster" << endl;
                assert(false);
            }
        }
        Timer::check();
    }
    auto to_drop1 = clusters_to_drop(clusters1);
    auto to_drop2 = clusters_to_drop(clusters2);
    vector<pair<Alignment, Alignment> > alns;
    int multimaps = 0;
    for (int i = 0; i < clusters1.size(); ++i) {
        auto& cluster1 = clusters1[i];
        auto& cluster2 = clusters2[i];
        // skip if we've filtered the cluster
        if ((cluster1.empty() || to_drop1.count(&cluster1))
            && (cluster2.empty() || to_drop2.count(&cluster2))) {
            continue;
        }
        // stop if we have enough multimaps
        if (multimaps > total_multimaps) { break; }
        // skip if we've got enough multimaps to get MQ and we're under the min cluster length
        if (min_cluster_length
            && cluster_coverage(cluster1) + cluster_coverage(cluster2)
            < min_cluster_length
            && alns.size() > 1) continue;
        alns.emplace_back();
        auto& p = alns.back();
        if (cluster1.size()) {
            p.first = align_cluster(read1, cluster1);
        } else {
            p.first = read1;
            p.first.clear_score();
            p.first.clear_identity();
            p.first.clear_path();
        }
        if (cluster2.size()) {
            p.second = align_cluster(read2, cluster2);
        } else {
            p.second = read2;
            p.second.clear_score();
            p.second.clear_identity();
            p.second.clear_path();
        }

        ++multimaps;
        
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) { cerr << "patch identities " << p.first.identity() << ", " << p.second.identity() << endl; }
        }
#endif
        Timer::check();
    }
    auto sort_and_dedup = [&](void) {
        // sort the aligned pairs
        std::sort(alns.begin(), alns.end(),
                  [&](const pair<Alignment, Alignment>& pair1,
                      const pair<Alignment, Alignment>& pair2) {
                      double bonus1=0, bonus2=0;
                      if (fragment_size) {
                          int dist1 = approx_fragment_length(pair1.first, pair1.second);
                          int dist2 = approx_fragment_length(pair2.first, pair2.second);
                          bonus1 = fragment_length_pdf(dist1) * cached_fragment_length_mean;
                          bonus2 = fragment_length_pdf(dist2) * cached_fragment_length_mean;
                      }
                      return (pair1.first.score() + pair1.second.score()) + bonus1
                      > (pair2.first.score() + pair2.second.score()) + bonus2;
                  });
        // remove duplicates (same score and same start position of both pairs)
        alns.erase(
            std::unique(
                alns.begin(), alns.end(),
                [&](const pair<Alignment, Alignment>& pair1,
                    const pair<Alignment, Alignment>& pair2) {
                    bool same = true;
                    if (pair1.first.score() && pair2.first.score()) {
                        same &= make_pos_t(pair1.first.path().mapping(0).position())
                            == make_pos_t(pair2.first.path().mapping(0).position());
                    }
                    if (pair1.second.score() && pair2.second.score()) {
                        same &= make_pos_t(pair1.second.path().mapping(0).position())
                            == make_pos_t(pair2.second.path().mapping(0).position());
                    }
                    if (!(pair1.first.score() && pair2.first.score()
                          || pair1.second.score() && pair2.second.score())) {
                        same = false;
                    }
                    return same;
                }),
            alns.end());
    };
    sort_and_dedup();
    if (fragment_size) {
        // go through the pairs and see if we need to rescue one side off the other
        bool rescued = false;
        int j = 0;
        for (auto& p : alns) {
            rescued |= pair_rescue(p.first, p.second);
            if (++j == mate_rescues) break;
        }
        if (rescued) sort_and_dedup();
    }
    
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            for (auto& p : alns) {
                auto& aln1 = p.first;
                cerr << "cluster aln 1 ------- " << pb2json(aln1) << endl;
                if (!check_alignment(aln1)) {
                    cerr << "alignment failure " << pb2json(aln1) << endl;
                    assert(false);
                }
                auto& aln2 = p.second;
                cerr << "cluster aln 2 ------- " << pb2json(aln2) << endl;
                if (!check_alignment(aln2)) {
                    cerr << "alignment failure " << pb2json(aln2) << endl;
                    assert(false);
                }
            }
        }
    }
#endif

    // calculate cluster mapping quality
    if (use_cluster_mq) {
        cluster_mq = compute_cluster_mapping_quality(clusters, read1.sequence().size() + read2.sequence().size());
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "cluster mq == " << cluster_mq << endl;
        }
#endif
    }

    // rebuild the thing we'll return
    int read1_max_score = 0;
    int read2_max_score = 0;
    for (auto& p : alns) {
        read1_max_score = max(p.first.score(), read1_max_score);
        read2_max_score = max(p.second.score(), read2_max_score);
        results.first.push_back(p.first);
        results.second.push_back(p.second);
    }
    compute_mapping_qualities(results, cluster_mq);

    // remove the extra pair used to compute mapping quality if necessary
    if (results.first.size() > max_multimaps) {
        results.first.resize(max_multimaps);
        results.second.resize(max_multimaps);
    }

    // mark primary and secondary
    for (int i = 0; i < results.first.size(); i++) {
        results.first[i].mutable_fragment_next()->set_name(read2.name());
        results.first[i].set_is_secondary(i > 0);
        results.second[i].mutable_fragment_prev()->set_name(read1.name());
        results.second[i].set_is_secondary(i > 0);
    }

    // optionally zap everything unless the primary alignments are individually top-scoring
    if (only_top_scoring_pair && results.first.size() &&
        (results.first[0].score() < read1_max_score ||
         results.second[0].score() < read2_max_score)) {
        results.first.clear();
        results.second.clear();
    }
    
    // we tried to align
    // if we don't have a fragment_size yet determined
    // and we didn't get a perfect, unambiguous hit on both reads
    // we'll need to try it again later when we do have a fragment_size
    // so store it in a buffer local to this mapper

    // tag the results with their fragment lengths
    // record the lengths in a deque that we use to keep a running estimate of the fragment length distribution
    // we then set the fragment_size cutoff using the moments of the estimated distribution
    bool imperfect_pair = false;
    for (int i = 0; i < min(results.first.size(), results.second.size()); ++i) {
        if (retrying) break;
        auto& aln1 = results.first.at(i);
        auto& aln2 = results.second.at(i);
        auto approx_frag_lengths = approx_pair_fragment_length(aln1, aln2);
        for (auto& j : approx_frag_lengths) {
            Path fragment;
            fragment.set_name(j.first);
            fragment.set_length(j.second);
            *aln1.add_fragment() = fragment;
            *aln2.add_fragment() = fragment;
            // if we have a perfect mapping, and we're under our hard fragment length cutoff
            // push the result into our deque of fragment lengths
            if (results.first.size() == 1
                && results.second.size() == 1
                && results.first.front().identity() > perfect_pair_identity_threshold
                && results.second.front().identity() > perfect_pair_identity_threshold
                && (fragment_size && abs(j.second) < fragment_size
                    || !fragment_size && abs(j.second) < fragment_max)) { // hard cutoff
                //cerr << "aln\tperfect alignments" << endl;
                record_fragment_configuration(j.second, aln1, aln2);
            } else if (!fragment_size) {
                imperfect_pair = true;
            }
        }
    }

    if (!retrying && imperfect_pair && fragment_max) {
        imperfect_pairs_to_retry.push_back(make_pair(read1, read2));
        results.first.clear();
        results.second.clear();
        // we signal the fact that this isn't a perfect pair, so we don't write it out externally?
        queued_resolve_later = true;
    }

    if(results.first.empty()) {
        results.first.push_back(read1);
        auto& aln = results.first.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }
    if(results.second.empty()) {
        results.second.push_back(read2);
        auto& aln = results.second.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }

    // Make sure to link up alignments even if they aren't mapped.
    for (auto& aln : results.first) {
        aln.set_name(read1.name());
        aln.mutable_fragment_next()->set_name(read2.name());
    }

    for (auto& aln : results.second) {
        aln.set_name(read2.name());
        aln.mutable_fragment_prev()->set_name(read1.name());
    }

    return results;

}

// rank the clusters by the number of unique read bases they cover
int cluster_coverage(const vector<MaximalExactMatch>& cluster) {
    set<string::const_iterator> seen;
    for (auto& mem : cluster) {
        string::const_iterator c = mem.begin;
        while (c != mem.end) seen.insert(c++);
    }
    return seen.size();
}

double Mapper::compute_cluster_mapping_quality(const vector<vector<MaximalExactMatch> >& clusters,
                                               int read_length) {
    if (clusters.size() == 0) {
        return 0;
    }
    if (clusters.size() == 1) {
        return { (double)max_cluster_mapping_quality };
    }
    vector<double> weights;
    for (auto& cluster : clusters) {
        weights.emplace_back();
        double& weight = weights.back();
        for (int i = 0; i < cluster.size(); ++i) {
            // for each mem, count half of its coverage with its neighbors towards this metric
            auto& mem = cluster[i];
            int shared_coverage = 0;
            if (i > 0) {
                auto& prev = cluster[i-1];
                if (prev.fragment == mem.fragment) {
                    shared_coverage += (prev.end <= mem.begin ? 0 : prev.end - mem.begin);
                }
            }
            if (i < cluster.size()-1) {
                auto& next = cluster[i+1];
                if (next.fragment == mem.fragment) {
                    shared_coverage += (mem.end <= next.begin ? 0 : mem.end - next.begin);
                }
            }
            weight +=
                (((double)mem.length() - (double)shared_coverage/2)
                 / read_length)
                / mem.match_count;
        }
        //cerr << "weight " << weight << endl;
    }
    // return the ratio between best and second best as quality
    std::sort(weights.begin(), weights.end(), std::greater<double>());
    // find how many maxes we have
    double max_weight = weights.front();
    int max_count = 0;
    while (max_weight == weights[max_count]) ++max_count;
    double best_chance = max_count > 1 ? prob_to_phred(1.0-(1.0/max_count)) : 0;
    if (weights[0] == 0) return 0;
    return min((double)max_cluster_mapping_quality,
               max(best_chance, prob_to_phred(weights[1]/weights[0])));
}

double
Mapper::average_node_length(void) {
    return (double) xindex->seq_length / (double) xindex->node_count;
}

bool mems_overlap(const MaximalExactMatch& mem1,
                  const MaximalExactMatch& mem2) {
    // we overlap if we are not completely separated
    return mem1.fragment == mem2.fragment
        && !(mem1.end <= mem2.begin
             || mem2.end <= mem1.begin);
}

int mems_overlap_length(const MaximalExactMatch& mem1,
                        const MaximalExactMatch& mem2) {
    if (!mems_overlap(mem1, mem2)) {
        return 0;
    } else {
        // overlap is length from min to max end/begin
        if (mem1.begin < mem2.begin) {
            if (mem1.end < mem2.end) {
                return mem2.end - mem1.begin;
            } else {
                return mem1.end - mem1.begin;
            }
        } else {
            if (mem2.end < mem1.end) {
                return mem1.end - mem2.begin;
            } else {
                return mem2.end - mem2.begin;
            }
        }
    }
}

bool clusters_overlap(const vector<MaximalExactMatch>& cluster1,
                      const vector<MaximalExactMatch>& cluster2) {
    for (auto& mem1 : cluster1) {
        for (auto& mem2 : cluster2) {
            if (mems_overlap(mem1, mem2)) {
                return true;
            }
        }
    }
    return false;
}

int sub_overlaps_of_first_aln(const vector<Alignment>& alns, float overlap_fraction) {
    // take the first
    // now look at the rest and measure overlap
    if (alns.empty()) return 0;
    auto& aln1 = alns.front();
    int seq_len = aln1.sequence().size();
    int overlaps = 0;
    for (int i = 1; i < alns.size(); ++i) {
        auto& aln2 = alns[i];
        // where the overlap is greater than overlap_fraction
        if ((double)query_overlap(aln1, aln2)/seq_len >= overlap_fraction) {
            ++overlaps;
        }
    }
    return overlaps;
}

set<const vector<MaximalExactMatch>* > Mapper::clusters_to_drop(const vector<vector<MaximalExactMatch> >& clusters) {
    set<const vector<MaximalExactMatch>* > to_drop;
    map<const vector<MaximalExactMatch>*, int> cluster_cov;
    for (auto& cluster : clusters) {
        cluster_cov[&cluster] = cluster_coverage(cluster);
    }
    for (int i = 0; i < clusters.size(); ++i) {
        // establish overlaps with longer clusters for all clusters
        auto& this_cluster = clusters[i];
        int t = cluster_cov[&this_cluster];
        int b = -1;
        int l = t;
        for (int j = i; j >= 0; --j) {
            if (j == i) continue;
            // are we overlapping?
            auto& other_cluster = clusters[j];
            if (clusters_overlap(this_cluster, other_cluster)) {
                int c = cluster_cov[&other_cluster];
                if (c > l) {
                    l = c;
                    b = j;
                }
            }
        }
        if (b >= 0 && (float) t / (float) l < drop_chain) {
            to_drop.insert(&this_cluster);
        }
    }
    return to_drop;
}

vector<Alignment>
Mapper::mems_pos_clusters_to_alignments(const Alignment& aln, vector<MaximalExactMatch>& mems, int additional_multimaps, double& cluster_mq) {

//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "mems for read " << mems_to_json(mems) << endl;
    }
//#endif
    
    int match;
    int gap_extension;
    int gap_open;
    if (aln.quality().empty() || !adjust_alignments_for_base_quality) {
        auto aligner =  get_regular_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    else {
        auto aligner =  get_qual_adj_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    
    int total_multimaps = max_multimaps + additional_multimaps;

    double avg_node_len = average_node_length();
    // go through the ordered single-hit MEMs
    // build the clustering model
    // find the alignments that are the best-scoring walks through it
    auto transition_weight = [&](const MaximalExactMatch& m1, const MaximalExactMatch& m2) {
        // find the difference in m1.end and m2.begin
        // find the positional difference in the graph between m1.end and m2.begin
        //int unique_coverage = (m1.end < m2.begin ? m1.length() + m2.length() : m2.end - m1.begin);
        int unique_coverage = m1.length() + m2.length() - mems_overlap_length(m1, m2);
        //cerr << "unique coverage " << unique_coverage << endl;
        //int overlap = (m1.end < m2.begin ? 0 : m1.end - m2.begin);
        pos_t m1_pos = make_pos_t(m1.nodes.front());
        pos_t m2_pos = make_pos_t(m2.nodes.front());
        double uniqueness = 2.0 / (m1.match_count + m2.match_count);

        // approximate distance by node lengths
        //int max_length = 2 * (m1.length() + m2.length());
        int max_length = aln.sequence().size();
        //int max_length = 10 * abs(m2.begin - m1.begin);
        //double approx_distance = (double) abs(id(m1_pos) - id(m2_pos)) * avg_node_len- offset(m1_pos);
        //double approx_distance = (double)abs(xindex->node_start(id(m1_pos))+offset(m1_pos) -
        int approx_dist = abs(approx_distance(m1_pos, m2_pos));
        
//#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "mems " << &m1 << ":" << m1 << " -> " << &m2 << ":" << m2 << "approx distance " << approx_dist << endl;
        }
//#endif
        if (approx_dist > max_length) {
            // too far
            return -std::numeric_limits<double>::max();
        } else {
            //int distance = graph_distance(m1_pos, m2_pos, max_length);
            int distance = approx_dist;
//#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "actual distance " << distance << endl;
            }
//#endif
            if (distance == max_length) {
                // couldn't find distance
                //distance = approx_dist;
                return -std::numeric_limits<double>::max();
            }
            if (is_rev(m1_pos) != is_rev(m2_pos)) {
                // disable inversions
                return -std::numeric_limits<double>::max();
            } else {
                // accepted transition
                double jump = abs((m2.begin - m1.begin) - distance);
                if (jump) {
                    return (double) unique_coverage * match * uniqueness - (gap_open + jump * gap_extension);
                } else {
                    return (double) unique_coverage * match * uniqueness;
                }
            }
        }
    };

    // build the model
    MEMChainModel markov_model({ aln.sequence().size() }, { mems }, this, transition_weight, aln.sequence().size());
    vector<vector<MaximalExactMatch> > clusters = markov_model.traceback(total_multimaps, false, debug);

    // don't attempt to align if we reach the maximum number of multimaps
    //if (clusters.size() == total_multimaps) clusters.clear();

    auto show_clusters = [&](void) {
        cerr << "clusters: " << endl;
        for (auto& cluster : clusters) {
            cerr << cluster.size() << " MEMs covering " << cluster_coverage(cluster) << " @ ";
            for (auto& mem : cluster) {
                size_t len = mem.begin - mem.end;
                for (auto& node : mem.nodes) {
                    id_t id = gcsa::Node::id(node);
                    size_t offset = gcsa::Node::offset(node);
                    bool is_rev = gcsa::Node::rc(node);
                    cerr << "|" << id << (is_rev ? "-" : "+") << ":" << offset << "," << mem.fragment << ",";
                    /*
                    for (auto& ref : node_positions_in_paths(gcsa::Node::encode(id, 0, is_rev))) {
                        auto& name = ref.first;
                        for (auto pos : ref.second) {
                            //cerr << name << (is_rev?"-":"+") << pos + offset;
                            cerr << "|" << id << (is_rev ? "-" : "+") << ":" << offset << ",";
                        }
                    }
                    */
                }
                cerr << mem.sequence() << " ";
            }
            cerr << endl;
        }
    };

//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "### clusters:" << endl;
            show_clusters();
        }
    }
//#endif

    if (use_cluster_mq) {
        cluster_mq = compute_cluster_mapping_quality(clusters, aln.sequence().size());
    }
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "cluster mapping quality " << cluster_mq << endl;
        }
    }
#endif
    auto to_drop = clusters_to_drop(clusters);

    // for up to our required number of multimaps
    // make the perfect-match alignment for the SMEM cluster
    // then fix it up with DP on the little bits between the alignments
    vector<Alignment> alns;
    int multimaps = 0;
    for (auto& cluster : clusters) {
        // filtered out due to overlap with longer chain
        if (to_drop.count(&cluster)) continue;
        if (++multimaps > total_multimaps) { break; }
        // skip this if we don't have sufficient cluster coverage and we have at least two alignments
        // which we can use to estimate mapping quality
        if (min_cluster_length && cluster_coverage(cluster) < min_cluster_length && alns.size() > 1) continue;
        // get the candidate graph
        // align to it
        Alignment candidate = align_cluster(aln, cluster);
        if (candidate.identity() > min_identity) {
            alns.emplace_back(candidate);
        }
    }

#pragma omp critical
    if (debug) {
        cerr << "alignments" << endl;
        for (auto& aln : alns) {
            cerr << aln.score();
            if (aln.score()) cerr << " pos1 " << aln.path().mapping(0).position().node_id() << " ";
            cerr << endl;
        }
    }

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            for (auto& aln : alns) {
                cerr << "cluster aln ------- " << pb2json(aln) << endl;
            }
            for (auto& aln : alns) {
                if (!check_alignment(aln)) {
                    cerr << "alignment failure " << pb2json(aln) << endl;
                    assert(false);
                }
            }
        }
    }
#endif
    // sort alignments by score
    // then by complexity (measured as number of edit operations)
    std::sort(alns.begin(), alns.end(),
              [&](const Alignment& a1, const Alignment& a2) {
                  return a1.score() > a2.score()
                      || a1.score() == a2.score()
                      && edit_count(a1) > edit_count(a2);
              });
    // remove likely perfect duplicates
    alns.erase(
        std::unique(
            alns.begin(), alns.end(),
            [&](const Alignment& aln1,
                const Alignment& aln2) {
                return
                    aln1.score() == aln2.score()
                    && (aln1.score() == 0
                        || make_pos_t(aln1.path().mapping(0).position())
                        == make_pos_t(aln2.path().mapping(0).position()));
            }),
        alns.end());
    return alns;
}

Alignment Mapper::align_maybe_flip(const Alignment& base, VG& graph, bool flip) {
    Alignment aln = base;
    if (flip) {
        aln.set_sequence(reverse_complement(base.sequence()));
        if (!base.quality().empty()) {
            aln.set_quality(base.quality());
            reverse(aln.mutable_quality()->begin(),
                    aln.mutable_quality()->end());
        }
    } else {
        aln.set_sequence(base.sequence());
        if (!base.quality().empty()) {
            aln.set_quality(base.quality());
        }
    }
    bool banded_global = false;
    bool pinned_alignment = false;
    bool pinned_reverse = false;
    aln = align_to_graph(aln,
                         graph,
                         max_query_graph_ratio,
                         pinned_alignment,
                         pinned_reverse,
                         full_length_alignment_bonus,
                         banded_global);
    aln.set_score(rescore_without_full_length_bonus(aln));
    if (flip) {
        aln = reverse_complement_alignment(
            aln,
            (function<int64_t(int64_t)>) ([&](int64_t id) {
                    return (int64_t)graph.get_node(id)->sequence().size();
                }));
    }
    return aln;
}

double Mapper::compute_uniqueness(const Alignment& aln, const vector<MaximalExactMatch>& mems) {
    // compute the per-base copy number of the alignment based on the MEMs in the cluster
    vector<int> v; v.resize(aln.sequence().size());
    auto aln_begin = aln.sequence().begin();
    for (auto& mem : mems) {
        // from the beginning to the end of the mem in the read
        // add the count of hits to each position
        for (int i = mem.begin - aln_begin; i < mem.end - aln_begin; ++i) {
            v[i] += mem.match_count;
        }
    }
    // what is the fraction of the cluster that is in repetitive mems
    // we calculate the uniqueness metric as the average number of hits for bases in the read covered by any MEM
    double repeated = std::accumulate(v.begin(), v.end(), 0.0,
                                      [](const int& a, const int& b) { return b > 1 ? a + 1 : a; });
    return repeated / aln.sequence().length();
}

Alignment Mapper::align_cluster(const Alignment& aln, const vector<MaximalExactMatch>& mems) {
    // poll the mems to see if we should flip
    int count_fwd = 0, count_rev = 0;
    for (auto& mem : mems) {
        bool is_rev = gcsa::Node::rc(mem.nodes.front());
        if (is_rev) {
            ++count_rev;
        } else {
            ++count_fwd;
        }
    }
    // get the graph
    VG graph = cluster_subgraph(aln, mems);
    // and test each direction for which we have MEM hits
    Alignment aln_fwd;
    Alignment aln_rev;
    if (count_fwd) {
        aln_fwd = align_maybe_flip(aln, graph, false);
    }
    if (count_rev) {
        aln_rev = align_maybe_flip(aln, graph, true);
    }
    if (aln_fwd.score() + aln_rev.score() == 0) {
        // abject failure, nothing aligned with score > 0
        Alignment result = aln;
        result.clear_path();
        result.clear_score();
        return result;
    } else if (aln_rev.score() > aln_fwd.score()) {
        // reverse won
        aln_rev.set_uniqueness(compute_uniqueness(aln, mems));
        return aln_rev;
    } else {
        // forward won
        aln_fwd.set_uniqueness(compute_uniqueness(aln, mems));
        return aln_fwd;
    }
}

void Mapper::cached_graph_context(VG& graph, const pos_t& pos, int length, LRUCache<id_t, Node>& node_cache, LRUCache<id_t, vector<Edge> >& edge_cache) {
    // walk the graph from this position forward
    // adding the nodes we run into to the graph
    set<pos_t> seen;
    set<pos_t> nexts;
    nexts.insert(pos);
    int distance = -offset(pos); // don't count what we won't traverse
    while (!nexts.empty()) {
        set<pos_t> todo;
        int nextd = 0;
        for (auto& next : nexts) {
            if (!seen.count(next)) {
                seen.insert(next);
                // add the node and its edges to the graph
                Node node = xg_cached_node(id(next), xindex, node_cache);
                nextd = nextd == 0 ? node.sequence().size() : min(nextd, (int)node.sequence().size());
                //distance += node.sequence().size();
                graph.add_node(node);
                for (auto& edge : xg_cached_edges_of(id(next), xindex, edge_cache)) {
                    graph.add_edge(edge);
                }
                // where to next
                for (auto& x : xg_cached_next_pos(next, true, xindex, node_cache, edge_cache)) {
                    todo.insert(x);
                }
                
                Timer::check();
            }
        }
        distance += nextd;
        if (distance > length) {
            break;
        }
        nexts = todo;
    }
    return;
}

VG Mapper::cluster_subgraph(const Alignment& aln, const vector<MaximalExactMatch>& mems) {
    auto& node_cache = get_node_cache();
    auto& edge_cache = get_edge_cache();
    assert(mems.size());
    auto& start_mem = mems.front();
    auto start_pos = make_pos_t(start_mem.nodes.front());
    auto rev_start_pos = reverse(start_pos, get_node_length(id(start_pos)));
    float expansion = 1.61803;
    int get_before = (int)(start_mem.begin - aln.sequence().begin()) * expansion;
    VG graph;
    if (get_before) {
        cached_graph_context(graph, rev_start_pos, get_before, node_cache, edge_cache);
    }
    for (int i = 0; i < mems.size(); ++i) {
        auto& mem = mems[i];
        auto pos = make_pos_t(mem.nodes.front());
        int get_after = expansion * (i+1 == mems.size() ? aln.sequence().end() - mem.begin
                                     : max(mem.length(), (int)(mems[i+1].begin - mem.begin)));
        //if (debug) cerr << pos << " " << mem << " getting after " << get_after << endl;
        cached_graph_context(graph, pos, get_after, node_cache, edge_cache);
    }
    graph.remove_orphan_edges();
    //if (debug) cerr << "graph " << pb2json(graph.graph) << endl;
    return graph;
}

VG Mapper::alignment_subgraph(const Alignment& aln, int context_size) {
    set<id_t> nodes;
    auto& path = aln.path();
    for (int i = 0; i < path.mapping_size(); ++i) {
        nodes.insert(path.mapping(i).position().node_id());
    }
    VG graph;
    for (auto& node : nodes) {
        *graph.graph.add_node() = xindex->node(node);
    }
    xindex->expand_context(graph.graph, max(1, context_size), false); // get connected edges
    graph.rebuild_indexes();
    return graph;
}

// estimate the fragment length as the difference in mean positions of both alignments
map<string, int> Mapper::approx_pair_fragment_length(const Alignment& aln1, const Alignment& aln2) {
    map<string, int> lengths;
    auto pos1 = alignment_mean_path_positions(aln1);
    auto pos2 = alignment_mean_path_positions(aln2);
    for (auto& p : pos1) {
        auto x = pos2.find(p.first);
        if (x != pos2.end()) {
            lengths[p.first] = x->second - p.second;
        }
    }
    return lengths;
}

void Mapper::record_fragment_configuration(int length, const Alignment& aln1, const Alignment& aln2) {
    // record the relative orientations
    assert(aln1.path().mapping(0).has_position() && aln2.path().mapping(0).has_position());
    bool aln1_is_rev = aln1.path().mapping(0).position().is_reverse();
    bool aln2_is_rev = aln2.path().mapping(0).position().is_reverse();
    bool same_orientation = aln1_is_rev == aln2_is_rev;
    fragment_orientations.push_front(same_orientation);
    if (fragment_orientations.size() > fragment_length_cache_size) {
        fragment_orientations.pop_back();
    }
    // assuming a dag-like graph
    // which direction do we go relative to the orientation of our first mate to find the second?
    bool same_direction = true;
    if (aln1_is_rev && length <= 0) {
        same_direction = true;
    } else if (!aln1_is_rev && length >= 0) {
        same_direction = true;
    } else if (aln1_is_rev && length >= 0) {
        same_direction = false;
    } else if (!aln1_is_rev && length <= 0) {
        same_direction = false;
    } else {
        assert(false);
    }
    fragment_directions.push_front(same_direction);
    if (fragment_directions.size() > fragment_length_cache_size) {
        fragment_directions.pop_back();
    }
    // assume we can record the fragment length
    fragment_lengths.push_front(abs(length));
    if (fragment_lengths.size() > fragment_length_cache_size) {
        auto last = fragment_lengths.back();
        fragment_lengths.pop_back();
    }
    if (++since_last_fragment_length_estimate > fragment_length_estimate_interval) {
        cached_fragment_length_mean = fragment_length_mean();
        cached_fragment_length_stdev = fragment_length_stdev();
        cached_fragment_orientation = fragment_orientation();
        cached_fragment_direction = fragment_direction();
        // set our fragment size cap to the cached mean + 10x the standard deviation
        fragment_size = cached_fragment_length_mean + fragment_sigma * cached_fragment_length_stdev;
        since_last_fragment_length_estimate = 1;
    }
}

double Mapper::fragment_length_stdev(void) {
    return stdev(fragment_lengths);
}

double Mapper::fragment_length_mean(void) {
    double sum = std::accumulate(fragment_lengths.begin(), fragment_lengths.end(), 0.0);
    return sum / fragment_lengths.size();
}

double Mapper::fragment_length_pdf(double length) {
    return normal_pdf(length, cached_fragment_length_mean, cached_fragment_length_stdev);
}

bool Mapper::fragment_orientation(void) {
    int count_same = 0;
    int count_diff = 0;
    for (auto& same_strand : fragment_orientations) {
        if (same_strand) ++count_same;
        else ++count_diff;
    }
    return count_same > count_diff;
}

bool Mapper::fragment_direction(void) {
    int count_fwd = 0;
    int count_rev = 0;
    for (auto& go_forward : fragment_directions) {
        if (go_forward) ++count_fwd;
        else ++count_rev;
    }
    return count_fwd > count_rev;
}

set<MaximalExactMatch*> Mapper::resolve_paired_mems(vector<MaximalExactMatch>& mems1,
                                                    vector<MaximalExactMatch>& mems2) {
    // find the MEMs that are within estimated_fragment_size of each other

    set<MaximalExactMatch*> pairable;

    // do a wide clustering and then do all pairs within each cluster
    // we will use these to determine the alignment strand
    //map<id_t, StrandCounts> node_strands;
    // records a mapping of id->MEMs, for cluster ranking
    map<id_t, vector<MaximalExactMatch*> > id_to_mems;
    // for clustering
    set<id_t> ids1, ids2;
    vector<id_t> ids;

    // run through the mems
    for (auto& mem : mems1) {
        for (auto& node : mem.nodes) {
            id_t id = gcsa::Node::id(node);
            id_to_mems[id].push_back(&mem);
            ids1.insert(id);
            ids.push_back(id);
        }
    }
    for (auto& mem : mems2) {
        for (auto& node : mem.nodes) {
            id_t id = gcsa::Node::id(node);
            id_to_mems[id].push_back(&mem);
            ids2.insert(id);
            ids.push_back(id);
        }
    }
    // remove duplicates
    //std::sort(ids.begin(), ids.end());
    //ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    // get each hit's path-relative position
    map<string, map<int, vector<id_t> > > node_positions;
    for (auto& id : ids) {
        for (auto& ref : node_positions_in_paths(gcsa::Node::encode(id, 0))) {
            auto& name = ref.first;
            for (auto pos : ref.second) {
                node_positions[name][pos].push_back(id);
            }
        }
    }

    vector<vector<id_t> > clusters;
    for (auto& g : node_positions) {
        //if (g.second.empty()) continue; // should be impossible
        //cerr << g.first << endl;
        clusters.emplace_back();
        int prev = -1;
        for (auto& x : g.second) {
            auto cluster = &clusters.back();
            //auto& prev = clusters.back().back();
            auto curr = x.first;
            if(debug) {
                cerr << "p/c " << prev << " " << curr << endl;
            }
            if (prev != -1) {
                if (curr - prev <= fragment_size) {
                    // in cluster
#ifdef debug_mapper
#pragma omp critical
                    {
                        if (debug) {
                            cerr << "in cluster" << endl;
                        }
                    }
#endif
                } else {
                    // It's a new cluster
                    clusters.emplace_back();
                    cluster = &clusters.back();
                }
            }
            //cerr << " " << x.first << endl;
            for (auto& y : x.second) {
                //cerr << "  " << y << endl;
                cluster->push_back(y);
            }
            prev = curr;
        }
    }

    for (auto& cluster : clusters) {
        // for each pair of ids in the cluster
        // which are not from the same read
        // estimate the distance between them
        // we're roughly in the expected range
        bool has_first = false;
        bool has_second = false;
        for (auto& id : cluster) {
            has_first |= ids1.count(id);
            has_second |= ids2.count(id);
        }
        if (!has_first || !has_second) continue;
        for (auto& id : cluster) {
            for (auto& memptr : id_to_mems[id]) {
                pairable.insert(memptr);
            }
        }
    }

    return pairable;
}

// We need a function to get the lengths of nodes, in case we need to
// reverse an Alignment, including all its Mappings and Positions.
int64_t Mapper::get_node_length(int64_t node_id) {
    // Grab the node sequence only from the XG index and get its size.
    // Make sure to use the cache
    return xg_cached_node_length(node_id, xindex, get_node_cache());
}

bool Mapper::check_alignment(const Alignment& aln) {
    // use the graph to extract the sequence
    // assert that this == the alignment
    if (aln.path().mapping_size()) {
        // get the graph corresponding to the alignment path
        Graph sub;
        for (int i = 0; i < aln.path().mapping_size(); ++ i) {
            auto& m = aln.path().mapping(i);
            if (m.has_position() && m.position().node_id()) {
                auto id = aln.path().mapping(i).position().node_id();
                // XXXXXX this is single-threaded!
                xindex->neighborhood(id, 2, sub);
            }
        }
        VG g; g.extend(sub);
        auto seq = g.path_string(aln.path());
        //if (aln.sequence().find('N') == string::npos && seq != aln.sequence()) {
        if (aln.quality().size() && aln.quality().size() != aln.sequence().size()) {
            cerr << "alignment quality is not the same length as its sequence" << endl
                 << pb2json(aln) << endl;
            return false;
        }
        if (seq != aln.sequence()) {
            cerr << "alignment does not match graph " << endl
                 << pb2json(aln) << endl
                 << "expect:\t" << aln.sequence() << endl
                 << "got:\t" << seq << endl;
            // save alignment
            write_alignment_to_file(aln, "fail-" + hash_alignment(aln) + ".gam");
            // save graph, bigger fragment
            xindex->expand_context(sub, 5, true);
            VG gn; gn.extend(sub);
            gn.serialize_to_file("fail-" + gn.hash() + ".vg");
            return false;
        }
    }
    return true;
}

Alignment Mapper::align_banded(const Alignment& read, int kmer_size, int stride, int max_mem_length, int band_width) {
    // split the alignment up into overlapping chunks of band_width size
    list<Alignment> alignments;
    // force used bandwidth to be divisible by 4
    // round up so we have > band_width
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "trying band width " << band_width << endl;
        }
    }
#endif
    if (band_width % 4) {
        band_width -= band_width % 4; band_width += 4;
    }
    assert(read.sequence().size() > band_width);
    int div = 2;
    while (read.sequence().size()/div > band_width) {
        ++div;
    }
    int segment_size = read.sequence().size()/div;
    // use segment sizes divisible by 4, as this simplifies math
    // round up as well
    // we'll divide the overlap by 2 and 2 and again when stripping from the start
    // and end of sub-reads
    if (segment_size % 4) {
        segment_size -= segment_size % 4; segment_size += 4;
    }
#ifdef debug_mapper
    if (debug) {
        cerr << "Segment size be " << segment_size << "/" << read.sequence().size() << endl;
    }
#endif
    // and overlap them too
    size_t to_align = div * 2 - 1; // number of alignments we'll do
    vector<pair<size_t, size_t>> to_strip; to_strip.resize(to_align);
    vector<Alignment> bands; bands.resize(to_align);

    // scan across the read choosing bands
    // these bands are hard coded to overlap by 50%
    // the last band is guaranteed to be segment_size long
    // overlap scheme example
    // read: ----------------------
    //       --------
    //           --------
    //               --------
    //                   --------
    //                     --------
    // Afterwards, we align each fragment, trim the overlaps implied by the layout
    // and concatenate the alignments. The result is a split read alignment that
    // can describe large indels, CNVs, and inversions natively, even though our
    // local alignment algorithm is only aware of alignment against DAGs.
    for (int i = 0; i < div; ++i) {
        size_t off = i*segment_size;
        // TODO: copying the whole read here, including sequence and qualities,
        // makes this O(n^2).
        Alignment aln = read;
        size_t addl_seq = 0;
        if (i+1 == div) {
            // ensure we have a full-length segment for the last alignment
            // otherwise we run the risk of trying to align a very tiny band
            size_t last_off = read.sequence().size() - segment_size;
            if (off > last_off) {
                // looks wrawng
                addl_seq = (off - last_off);
                aln.set_sequence(read.sequence().substr(last_off));
                //assert(aln.sequence().size() == segment_size);
            } else {
                aln.set_sequence(read.sequence().substr(off));
            }
        } else {
            aln.set_sequence(read.sequence().substr(off, segment_size));
        }
        size_t idx = 2*i;
        to_strip[idx].first = (i == 0 ? 0 : segment_size/4 + addl_seq);
        to_strip[idx].second = (i+1 == div ? 0 : segment_size/4);
        bands[idx] = aln;
        if (i != div-1) { // if we're not at the last sequence
            aln.set_sequence(read.sequence().substr(off+segment_size/2,
                                                    segment_size));
            idx = 2*i+1;
            to_strip[idx].first = segment_size/4;
            // record second but take account of case where we run off end
            to_strip[idx].second = segment_size/4 - (segment_size - aln.sequence().size());
            bands[idx] = aln;
        }
    }

    vector<vector<Alignment>> multi_alns;
    vector<Alignment> alns;
    if (max_multimaps > 1) multi_alns.resize(to_align);
    else alns.resize(to_align);

    auto do_band = [&](int i) {
        if (max_multimaps > 1) {
            vector<Alignment>& malns = multi_alns[i];
            double cluster_mq = 0;
            malns = align_multi_internal(false, bands[i], kmer_size, stride, max_mem_length, band_width, cluster_mq, extra_multimaps, nullptr);
            // always include an unaligned mapping
            malns.push_back(bands[i]);
            for (vector<Alignment>::iterator a = malns.begin(); a != malns.end(); ++a) {
                Alignment& aln = *a;
                bool above_threshold = aln.identity() >= min_identity;
                if (!above_threshold) {
                    // treat as unmapped
                    aln = bands[i];
                }
                // strip overlaps
                aln = strip_from_start(aln, to_strip[i].first);
                aln = strip_from_end(aln, to_strip[i].second);
            }
        } else {
            Alignment& aln = alns[i];
            aln = align(bands[i], kmer_size, stride, max_mem_length, band_width);
            bool above_threshold = aln.identity() >= min_identity;
            if (!above_threshold) {
                aln = bands[i]; // unmapped
            }
            
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) {
                    cerr << "Unstripped alignment: " << pb2json(aln) << endl;
                }
            }
#endif
            
            // strip overlaps
            //cerr << "checking before strip" << endl;
            //check_alignment(aln);
            // clean up null positions that confuse stripping
            for (int j = 0; j < aln.path().mapping_size(); ++j) {
                auto* mapping = aln.mutable_path()->mutable_mapping(j);
                if (mapping->has_position() && !mapping->position().node_id()) {
                    mapping->clear_position();
                }
            }
            aln = strip_from_start(aln, to_strip[i].first);
            aln = strip_from_end(aln, to_strip[i].second);
            //cerr << "checking after strip" << endl;
            //check_alignment(aln);
            //cerr << "OK" << endl;
        }
    };
    
    if (alignment_threads > 1) {
#pragma omp parallel for schedule(dynamic,1)
        for (int i = 0; i < bands.size(); ++i) {
            do_band(i);
        }
    } else {
        for (int i = 0; i < bands.size(); ++i) {
            do_band(i);
        }
    }

    // resolve the highest-scoring traversal of the multi-mappings
    if (max_multimaps > 1) {
        alns = resolve_banded_multi(multi_alns);
        multi_alns.clear(); // clean up
    }

    // check that the alignments are valid
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            for (auto& aln : alns) {
                check_alignment(aln);
            }
        }
    }
#endif

    // merge the resulting alignments
    Alignment merged = merge_alignments(alns);

    merged.set_score(score_alignment(merged));
    merged.set_identity(identity(merged.path()));
    merged.set_quality(read.quality());
    merged.set_name(read.name());

    if(debug) {
        for(int i = 0; i < merged.path().mapping_size(); i++) {
            // Check each Mapping to make sure it doesn't go past the end of its
            // node.
            auto& mapping = merged.path().mapping(i);

            // What node is the mapping on
            int64_t node_id = mapping.position().node_id();
            if(node_id != 0) {
                // If it's actually on a node, get the node's sequence length
                int64_t node_length = get_node_length(node_id);

                // Make sure the mapping from length is shorter than the node length
                assert(node_length >= mapping_from_length(mapping));
            }
        }
    }

    return merged;
}

vector<Alignment> Mapper::resolve_banded_multi(vector<vector<Alignment>>& multi_alns) {
    // use a basic dynamic programming to score the path through the multi mapping
    // we add the score as long as our alignments overlap (we expect them to)
    // otherwise we add nothing
    // reads that are < the minimum alignment score threshold are dropped

    // a vector of
    // score, current alignment, parent alignment (direction)
    typedef tuple<int, Alignment*, size_t> score_t;
    vector<vector<score_t>> scores;
    scores.resize(multi_alns.size());
    // start with the scores for the first alignments
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "resolving banded multi over:" << endl;
            for (auto& alns : multi_alns) {
                for (auto& aln : alns) {
                    if (aln.has_path()) {
                        cerr << aln.score() << "@ " << make_pos_t(aln.path().mapping(0).position()) <<", ";
                    }
                }
                cerr << endl;
            }
        }
    }
#endif
    for (auto& aln : multi_alns[0]) {
        scores.front().push_back(make_tuple(aln.score(), &aln, 0));
    }
    for (size_t i = 1; i < multi_alns.size(); ++i) {
        auto& curr_alns = multi_alns[i];
        vector<score_t>& curr_scores = scores[i];
        auto& prev_scores = scores[i-1];
        // find the best previous score
        score_t best_prev = prev_scores.front();
        size_t best_idx = 0;
        score_t unmapped_prev = prev_scores.front();
        size_t unmapped_idx = 0;
        size_t j = 0;
        for (auto& t : prev_scores) {
            if (get<0>(t) > get<0>(best_prev)) {
                best_prev = t;
                best_idx = j;
            }
            if (get<0>(t) == 0) {
                unmapped_idx = j;
                unmapped_prev = t;
            }
            ++j;
        }
        // for each alignment
        for (auto& aln : curr_alns) {
            // if it's not mapped, take the best previous score
            if (!aln.score()) {
                curr_scores.push_back(make_tuple(get<0>(best_prev),
                                                 &aln, best_idx));
            } else {
                // determine our start
                auto& curr_start = aln.path().mapping(0).position();
                // accumulate candidate alignments
                map<int, vector<pair<score_t, size_t>>> candidates;
                // for each previous alignment
                size_t k = 0;
                for (auto& score : prev_scores) {
                    auto old = get<1>(score);
                    if (!old->score()) continue; // unmapped
                    auto prev_end = path_end(old->path());
                    // save it as a candidate if the two are adjacent
                    // and in the same orientation
                    if (adjacent_positions(prev_end, curr_start)) {
                        candidates[get<0>(score)].push_back(make_pair(score,k));
                    }
                    ++k;
                }
                if (candidates.size()) {
                    // take the best one (at least the first best one we saw)
                    auto& opt = candidates.rbegin()->second.front();
                    // DP scoring step: add scores when we match head to tail
                    curr_scores.push_back(make_tuple(get<0>(opt.first) + aln.score(),
                                                     &aln, opt.second));
                } else {
                    // if there are no alignments matching our start
                    // just take the highest-scoring one
                    auto best_prev_aln = get<1>(prev_scores[best_idx]);
                    if (best_prev_aln->has_path()) {
                        curr_scores.push_back(make_tuple(get<0>(best_prev),
                                                         &aln, best_idx));
                    } else {
                        curr_scores.push_back(make_tuple(get<0>(unmapped_prev),
                                                         &aln, unmapped_idx));
                    }
                }
            }
        }
    }
    // find the best score at the end
    score_t best_last = scores.back().front();
    size_t best_last_idx = 0;
    size_t j = 0;
    for (auto& s : scores.back()) {
        if (get<0>(s) > get<0>(best_last)) {
            best_last = s;
            best_last_idx = j;
        }
        ++j;
    }
    // accumulate the alignments in the optimal path
    vector<Alignment> alns; alns.resize(multi_alns.size());
    size_t prev_best_idx = best_last_idx;
    for (int i = scores.size()-1; i >= 0; --i) {
        auto& score = scores[i][prev_best_idx];
        alns[i] = *get<1>(score); // save the alignment
        prev_best_idx = get<2>(score); // and where we go next
    }
    return alns;
}

bool Mapper::adjacent_positions(const Position& pos1, const Position& pos2) {
    // are they the same id, with offset differing by 1?
    if (pos1.node_id() == pos2.node_id()
        && pos1.offset() == pos2.offset()-1) {
        return true;
    }
    // otherwise, we're going to need to check via the index
    VG graph;
    // pick up a graph that's just the neighborhood of the start and end positions
    int64_t id1 = pos1.node_id();
    int64_t id2 = pos2.node_id();
    if(xindex) {
        // Grab the node sequence only from the XG index and get its size.
        xindex->get_id_range(id1, id1, graph.graph);
        xindex->get_id_range(id2, id2, graph.graph);
        xindex->expand_context(graph.graph, 1, false);
        graph.rebuild_indexes();
    } else if(index) {
        index->get_context(id1, graph);
        index->get_context(id2, graph);
        index->expand_context(graph, 1);
    } else {
        throw runtime_error("No index to get nodes from.");
    }
    // now look in the graph to figure out if we are adjacent
    return graph.adjacent(pos1, pos2);
}

QualAdjAligner* Mapper::get_qual_adj_aligner(void) {
    int tid = qual_adj_aligners.size() > 1 ? omp_get_thread_num() : 0;
    return qual_adj_aligners[tid];
}

Aligner* Mapper::get_regular_aligner(void) {
    int tid = regular_aligners.size() > 1 ? omp_get_thread_num() : 0;
    return regular_aligners[tid];
}

LRUCache<id_t, Node>& Mapper::get_node_cache(void) {
    int tid = node_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *node_cache[tid];
}

LRUCache<id_t, size_t>& Mapper::get_node_start_cache(void) {
    int tid = node_start_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *node_start_cache[tid];
}

LRUCache<gcsa::node_type, map<string, vector<size_t> > >& Mapper::get_node_pos_cache(void) {
    int tid = node_pos_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *node_pos_cache[tid];
}

LRUCache<id_t, vector<Edge> >& Mapper::get_edge_cache(void) {
    int tid = edge_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *edge_cache[tid];
}

void Mapper::compute_mapping_qualities(vector<Alignment>& alns, double cluster_mq) {
    if (alns.empty()) return;

    auto aligner = (alns.front().quality().empty() ? (BaseAligner*) get_regular_aligner() : (BaseAligner*) get_qual_adj_aligner());
    int sub_overlaps = sub_overlaps_of_first_aln(alns, mq_overlap);
    switch (mapping_quality_method) {
        case Approx:
            aligner->compute_mapping_quality(alns, max_mapping_quality, true, cluster_mq, use_cluster_mq, sub_overlaps);
            break;
        case Exact:
            aligner->compute_mapping_quality(alns, max_mapping_quality, false, cluster_mq, use_cluster_mq, sub_overlaps);
            break;
        default: // None
            break;
    }
}
    
void Mapper::compute_mapping_qualities(pair<vector<Alignment>, vector<Alignment>>& pair_alns, double cluster_mq) {
    if (pair_alns.first.empty() || pair_alns.second.empty()) return;
    auto aligner = (pair_alns.first.front().quality().empty() ? (BaseAligner*) get_regular_aligner() : (BaseAligner*) get_qual_adj_aligner());
    int sub_overlaps1 = sub_overlaps_of_first_aln(pair_alns.first, mq_overlap);
    int sub_overlaps2 = sub_overlaps_of_first_aln(pair_alns.second, mq_overlap);
    switch (mapping_quality_method) {
        case Approx:
            aligner->compute_paired_mapping_quality(pair_alns, max_mapping_quality, true, cluster_mq, use_cluster_mq, sub_overlaps1, sub_overlaps2);
            break;
        case Exact:
            aligner->compute_paired_mapping_quality(pair_alns, max_mapping_quality, false, cluster_mq, use_cluster_mq, sub_overlaps1, sub_overlaps2);
            break;
        default: // None
            break;
    }
}

vector<Alignment> Mapper::score_sort_and_deduplicate_alignments(vector<Alignment>& all_alns, const Alignment& original_alignment) {
    if (all_alns.size() == 0) {
        all_alns.emplace_back();
        Alignment& aln = all_alns.back();
        aln = original_alignment;
        aln.clear_path();
        aln.set_score(0);
        return all_alns;
    }
    
    map<int, set<Alignment*> > alignment_by_score;
    for (auto& ta : all_alns) {
        Alignment* aln = &ta;
        alignment_by_score[aln->score()].insert(aln);
    }
    // TODO: Filter down subject to a minimum score per base or something?
    // Collect all the unique alignments (to compute mapping quality) and order by score
    vector<Alignment> sorted_unique_alignments;
    for(auto it = alignment_by_score.rbegin(); it != alignment_by_score.rend(); ++it) {
        // Copy over all the alignments in descending score order (following the pointers into the "alignments" vector)
        // Iterating through a set keyed on ints backward is in descending order.
        
        // This is going to let us deduplicate our alignments with this score, by storing them serialized to strings in this set.
        set<string> serializedAlignmentsUsed;
        
        for(Alignment* pointer : (*it).second) {
            // We serialize the alignment to a string
            string serialized;
            pointer->SerializeToString(&serialized);
            
            if(!serializedAlignmentsUsed.count(serialized)) {
                // This alignment hasn't been produced yet. Produce it. The
                // order in the alignment vector doesn't matter for things with
                // the same score.
                sorted_unique_alignments.push_back(*pointer);
                
                // Save it so we can avoid putting it in the vector again
                serializedAlignmentsUsed.insert(serialized);
            }
        }
    }
    return sorted_unique_alignments;
}

// filters down to requested number of alignments and marks
void Mapper::filter_and_process_multimaps(vector<Alignment>& sorted_unique_alignments, int additional_multimaps) {
    int total_multimaps = max_multimaps + additional_multimaps;
    if (sorted_unique_alignments.size() > total_multimaps){
        sorted_unique_alignments.resize(total_multimaps);
    }
    
    // TODO log best alignment score?
    for(size_t i = 0; i < sorted_unique_alignments.size(); i++) {
        // Mark all but the first, best alignment as secondary
        sorted_unique_alignments[i].set_is_secondary(i > 0);
    }
}
    
vector<Alignment> Mapper::align_multi(const Alignment& aln, int kmer_size, int stride, int max_mem_length, int band_width) {
    double cluster_mq = 0;
    return align_multi_internal(true, aln, kmer_size, stride, max_mem_length, band_width, cluster_mq, extra_multimaps, nullptr);
}
    
vector<Alignment> Mapper::align_multi_internal(bool compute_unpaired_quality,
                                               const Alignment& aln,
                                               int kmer_size, int stride,
                                               int max_mem_length,
                                               int band_width,
                                               double& cluster_mq,
                                               int additional_multimaps,
                                               vector<MaximalExactMatch>* restricted_mems) {
    
    if(debug) {
#pragma omp critical
        cerr << "align_multi_internal("
            << compute_unpaired_quality << ", " 
            << aln.sequence() << ", " 
            << kmer_size << ", " 
            << stride << ", " 
            << band_width << ", " 
            << additional_multimaps << ", " 
            << restricted_mems << ")" 
            << endl;
        if (aln.has_path()) {
            // if we're realigning, show in the debugging output what we start with
            cerr << pb2json(aln) << endl;
        }
    }
    
    // trigger a banded alignment if we need to
    // note that this will in turn call align_multi_internal on fragments of the read
    if (aln.sequence().size() > band_width) {
        // TODO: banded alignment currently doesn't support mapping qualities because it only produces one alignment
#ifdef debug_mapper
#pragma omp critical
        if (debug) cerr << "switching to banded alignment" << endl;
#endif
        return vector<Alignment>{align_banded(aln, kmer_size, stride, max_mem_length, band_width)};
    }
    
    // try to get at least 2 multimaps so that we can calculate mapping quality
    int additional_multimaps_for_quality;
    if (additional_multimaps == 0 && max_multimaps == 1 && mapping_quality_method != None) {
        additional_multimaps_for_quality = 1;
    }
    else {
        additional_multimaps_for_quality = additional_multimaps;
    }

    vector<Alignment> alignments;
    if (kmer_size || xindex == nullptr) {
        // if we've defined a kmer size, use the legacy style mapper
        alignments = align_multi_kmers(aln, kmer_size, stride, band_width);
    }
    else {
        // otherwise use the mem mapper, which is a banded multi mapper by default
        
        // use pre-restricted mems for paired mapping or find mems here
        if (restricted_mems != nullptr) {
            // mem hits will already have been queried
            alignments = align_mem_multi(aln, *restricted_mems, cluster_mq, additional_multimaps_for_quality);
        }
        else {
            vector<MaximalExactMatch> mems = find_mems_deep(aln.sequence().begin(),
                                                            aln.sequence().end(),
                                                            max_mem_length,
                                                            min_mem_length,
                                                            mem_reseed_length);
            // query mem hits

            alignments = align_mem_multi(aln, mems, cluster_mq, additional_multimaps_for_quality);
        }
    }
    
    alignments = score_sort_and_deduplicate_alignments(alignments, aln);
    
    // compute mapping quality before removing extra alignments
    if (compute_unpaired_quality) {
        compute_mapping_qualities(alignments, cluster_mq);
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "mapping quality " << alignments.front().mapping_quality() << " " << cluster_mq << endl;
        }
#endif
        filter_and_process_multimaps(alignments, 0);
    } else {
        filter_and_process_multimaps(alignments, additional_multimaps);
    }
    
    for (auto& aln : alignments) {
        // Make sure no alignments are wandering out of the graph
        for (size_t i = 0; i < aln.path().mapping_size(); i++) {
            // Look at each mapping
            auto& mapping = aln.path().mapping(i);
            
            if (mapping.position().node_id()) {
                // Get the size of its node from whatever index we have
                size_t node_size = get_node_length(mapping.position().node_id());
                
                // Make sure the mapping fits in the node
                assert(mapping.position().offset() + mapping_from_length(mapping) <= node_size);
            }
        }
    }
    
    return alignments;
}

vector<Alignment> Mapper::align_multi_kmers(const Alignment& aln, int kmer_size, int stride, int band_width) {

    std::chrono::time_point<std::chrono::system_clock> start_both, end_both;
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) start_both = std::chrono::system_clock::now();
    }
#endif
    const string& sequence = aln.sequence();
    
    // we assume a kmer size to be specified
    if (!kmer_size && !kmer_sizes.empty()) {
        // basically assumes one kmer size
        kmer_size = *kmer_sizes.begin();
    }
    assert(kmer_size);
    // and start with stride such that we barely cover the read with kmers
    if (stride == 0)
        stride = sequence.size()
            / ceil((double)sequence.size() / kmer_size);

    int kmer_hit_count = 0;
    int kept_kmer_count = 0;

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "aligning " << aln.sequence() << endl;
    }
#endif

    // This will hold the best forward alignment (or an alignment with no path and 0 score if no alignment is found).
    Alignment best_f = aln;

    // This will hold all of the forward alignments up to max_multimaps
    vector<Alignment> alignments_f;

    // This will similarly hold all the reverse alignments.
    // Right now we set it up to provide input to the actual alignment algorithm.
    Alignment best_r = reverse_complement_alignment(aln,
                                                    (function<int64_t(int64_t)>) ([&](int64_t id) { return get_node_length(id); }));
    // This will hold all of the reverse alignments up to max_multimaps
    vector<Alignment> alignments_r;

    auto increase_sensitivity = [this,
                                 &kmer_size,
                                 &stride,
                                 &sequence,
                                 &best_f,
                                 &best_r]() {
        kmer_size -= kmer_sensitivity_step;
        stride = sequence.size() / ceil( (double)sequence.size() / kmer_size);
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "realigning with " << kmer_size << " " << stride << endl;
        }
#endif
    };

    int attempt = 0;
    int kmer_count_f = 0;
    int kmer_count_r = 0;

    while (!(best_f.identity() > min_identity
             || best_r.identity() > min_identity)
           && attempt < max_attempts) {

        {
            std::chrono::time_point<std::chrono::system_clock> start, end;
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) start = std::chrono::system_clock::now();
            }
#endif
            // Go get all the forward alignments, putting the best one in best_f.
            alignments_f = align_threaded(best_f, kmer_count_f, kmer_size, stride, attempt);
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) {
                    end = std::chrono::system_clock::now();
                    std::chrono::duration<double> elapsed_seconds = end-start;
                    cerr << elapsed_seconds.count() << "\t" << "+" << "\t" << best_f.sequence() << endl;
                }
            }
#endif
        }

        if (!(prefer_forward && best_f.identity() >= accept_identity))
        {
            // If we need to look on the reverse strand, do that too.
            std::chrono::time_point<std::chrono::system_clock> start, end;
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) start = std::chrono::system_clock::now();
            }
#endif
            auto alns =  align_threaded(best_r, kmer_count_r, kmer_size, stride, attempt);
            alignments_r = reverse_complement_alignments(alns,
                                                         (function<int64_t(int64_t)>) ([&](int64_t id) { return get_node_length(id); }));
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) {
                    end = std::chrono::system_clock::now();
                    std::chrono::duration<double> elapsed_seconds = end-start;
                    cerr << elapsed_seconds.count() << "\t" << "-" << "\t" << best_r.sequence() << endl;
                }
            }
#endif
        }

        ++attempt;

        if (best_f.score() == 0 && best_r.score() == 0
            && kmer_size - kmer_sensitivity_step >= kmer_min) {
            // We couldn't find anything. Try harder.
            increase_sensitivity();
        } else {
            // We found at least one alignment
            break;
        }

    }

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            end_both = std::chrono::system_clock::now();
            std::chrono::duration<double> elapsed_seconds = end_both-start_both;
            cerr << elapsed_seconds.count() << "\t" << "b" << "\t" << sequence << endl;
        }
    }
#endif
    
    // merge return all alignments found, don't bother sorting here
    vector<Alignment> merged;
    merged.reserve(alignments_f.size() + alignments_r.size());
    for (int i = 0; i < alignments_f.size(); i++) {
        merged.push_back(alignments_f[i]);
    }
    for (int i = 0; i < alignments_r.size(); i++) {
        merged.push_back(alignments_r[i]);
    }
    
    // Return the merged list of good alignments. Does not bother updating the input alignment.
    return merged;
}



Alignment Mapper::align(const Alignment& aln, int kmer_size, int stride, int max_mem_length, int band_width) {
    // TODO computing mapping quality could be inefficient depending on the method chosen
    
    // Do the multi-mapping
    vector<Alignment> best = align_multi(aln, kmer_size, stride, max_mem_length, band_width);

    if(best.size() == 0) {
        // Spit back an alignment that says we failed, but make sure it has the right sequence in it.
        Alignment failed = aln;
        failed.clear_path();
        failed.set_score(0);
        return failed;
    }

    // Otherwise, just report the best alignment, since we know one exists
    return best[0];
}

set<pos_t> gcsa_nodes_to_positions(const vector<gcsa::node_type>& nodes) {
    set<pos_t> positions;
    for(gcsa::node_type node : nodes) {
        positions.insert(make_pos_t(node));
    }
    return positions;    
}

set<pos_t> Mapper::sequence_positions(const string& seq) {
    gcsa::range_type gcsa_range = gcsa->find(seq);
    std::vector<gcsa::node_type> gcsa_nodes;
    gcsa->locate(gcsa_range, gcsa_nodes);
    return gcsa_nodes_to_positions(gcsa_nodes);
}

// Use the GCSA2 index to find super-maximal exact matches.
vector<MaximalExactMatch>
Mapper::find_mems_simple(string::const_iterator seq_begin,
                         string::const_iterator seq_end,
                         int max_mem_length,
                         int min_mem_length,
                         int reseed_length) {

    if (!gcsa) {
        cerr << "error:[vg::Mapper] a GCSA2 index is required to query MEMs" << endl;
        exit(1);
    }

    string::const_iterator cursor = seq_end;
    vector<MaximalExactMatch> mems;

    // an empty sequence matches the entire bwt
    if (seq_begin == seq_end) {
        mems.emplace_back(
            MaximalExactMatch(seq_begin, seq_end,
                              gcsa::range_type(0, gcsa->size() - 1)));
        return mems;
    }
    
    // find SMEMs using GCSA+LCP array
    // algorithm sketch:
    // set up a cursor pointing to the last position in the sequence
    // set up a structure to track our MEMs, and set it == "" and full range match
    // while our cursor is >= the beginning of the string
    //   try a step of backwards searching using LF mapping
    //   if our range goes to 0
    //       go back to the last non-empty range
    //       emit the MEM corresponding to this range
    //       start a new mem
    //           use the LCP array's parent function to cut off the end of the match
    //           (effectively, this steps up the suffix tree)
    //           and calculate the new end point using the LCP of the parent node
    // emit the final MEM, if we finished in a matching state

    // the temporary MEM we'll build up in this process
    auto full_range = gcsa::range_type(0, gcsa->size() - 1);
    MaximalExactMatch match(cursor, cursor, full_range);
    gcsa::range_type last_range = match.range;
    --cursor; // start off looking at the last character in the query
    while (cursor >= seq_begin) {
        // hold onto our previous range
        last_range = match.range;
        // execute one step of LF mapping
        match.range = gcsa->LF(match.range, gcsa->alpha.char2comp[*cursor]);
        if (gcsa::Range::empty(match.range)
            || max_mem_length && match.end-cursor > max_mem_length
            || match.end-cursor > gcsa->order()) {
            // break on N; which for DNA we assume is non-informative
            // this *will* match many places in assemblies; this isn't helpful
            if (*cursor == 'N' || last_range == full_range) {
                // we mismatched in a single character
                // there is no MEM here
                match.begin = cursor+1;
                match.range = last_range;
                mems.push_back(match);
                match.end = cursor;
                match.range = full_range;
                --cursor;
            } else {
                // we've exhausted our BWT range, so the last match range was maximal
                // or: we have exceeded the order of the graph (FPs if we go further)
                //     we have run over our parameter-defined MEM limit
                // record the last MEM
                match.begin = cursor+1;
                match.range = last_range;
                mems.push_back(match);
                // set up the next MEM using the parent node range
                // length of last MEM, which we use to update our end pointer for the next MEM
                size_t last_mem_length = match.end - match.begin;
                // get the parent suffix tree node corresponding to the parent of the last MEM's STNode
                gcsa::STNode parent = lcp->parent(last_range);
                // change the end for the next mem to reflect our step size
                size_t step_size = last_mem_length - parent.lcp();
                match.end = mems.back().end-step_size;
                // and set up the next MEM using the parent node range
                match.range = parent.range();
            }
        } else {
            // we are matching
            match.begin = cursor;
            // just step to the next position
            --cursor;
        }
    }
    // if we have a non-empty MEM at the end, record it
    if (match.end - match.begin > 0) mems.push_back(match);

    // find the SMEMs from the mostly-SMEM and some MEM list we've built
    // FIXME: un-hack this (it shouldn't be needed!)
    // the algorithm sometimes generates MEMs contained in SMEMs
    // with the pattern that they have the same beginning position
    map<string::const_iterator, string::const_iterator> smems_begin;
    for (auto& mem : mems) {
        auto x = smems_begin.find(mem.begin);
        if (x == smems_begin.end()) {
            smems_begin[mem.begin] = mem.end;
        } else {
            if (x->second < mem.end) {
                x->second = mem.end;
            }
        }
    }
    // remove zero-length entries and MEMs that aren't SMEMs
    // the zero-length ones are associated with single-base MEMs that tend to
    // match the entire index (typically Ns)
    // minor TODO: fix the above algorithm so they aren't introduced at all
    mems.erase(std::remove_if(mems.begin(), mems.end(),
                              [&smems_begin,
                               &min_mem_length](const MaximalExactMatch& m) {
                                  return ( m.end-m.begin == 0
                                           || m.length() < min_mem_length
                                           || smems_begin[m.begin] != m.end
                                           || m.count_Ns() > 0
                                      );
                              }),
               mems.end());
    // return the matches in natural order
    std::reverse(mems.begin(), mems.end());

    // fill the counts before deciding what to do
    for (auto& mem : mems) {
        if (mem.length() >= min_mem_length) {
            mem.match_count = gcsa->count(mem.range);
            if (mem.match_count > 0 && (!hit_max || mem.match_count <= hit_max)) {
                gcsa->locate(mem.range, mem.nodes);
            }
        }
    }
    
    // reseed the long smems with shorter mems
    if (reseed_length) {
        // find if there are any mems that should be reseeded
        // iterate through MEMs
        vector<MaximalExactMatch> reseeded;
        for (auto& mem : mems) {
            // reseed if we have a long singular match
            if (mem.length() >= reseed_length
                && mem.match_count == 1
                // or if we only have one mem for the entire read (even if it may have many matches)
                || mems.size() == 1) {
                // reseed at midway between here and the min mem length and at the min mem length
                int reseed_to = mem.length() / 2;
                int reseeds = 0;
                while (reseeds == 0 && reseed_to >= min_mem_length) {
#ifdef debug_mapper
#pragma omp critical
                    if (debug) cerr << "reseeding " << mem.sequence() << " with " << reseed_to << endl;
#endif
                    vector<MaximalExactMatch> remems = find_mems_simple(mem.begin,
                                                                        mem.end,
                                                                        reseed_to,
                                                                        min_mem_length,
                                                                        0);
                    reseed_to /= 2;
                    for (auto& rmem : remems) {
                        // keep if we have more than the match count of the parent
                        if (rmem.length() >= min_mem_length
                            && rmem.match_count > mem.match_count) {
                            ++reseeds;
                            reseeded.push_back(rmem);
                        }
                    }
                }
                // at least keep the original mem if needed
                if (reseeds == 0) {
                    reseeded.push_back(mem);
                }
            } else {
                reseeded.push_back(mem);
            }
        }
        mems = reseeded;
        // re-sort the MEMs by their start position
        std::sort(mems.begin(), mems.end(), [](const MaximalExactMatch& m1, const MaximalExactMatch& m2) { return m1.begin < m2.begin; });
    }
    // print the matches
    /*
    for (auto& mem : mems) {
        cerr << mem << endl;
    }
    */
    // verify the matches (super costly at scale)
    /*
#ifdef debug_mapper
    if (debug) { check_mems(mems); }
#endif
    */
    return mems;
}

// Use the GCSA2 index to find super-maximal exact matches (and optionally sub-MEMs).
vector<MaximalExactMatch> Mapper::find_mems_deep(string::const_iterator seq_begin,
                                                 string::const_iterator seq_end,
                                                 int max_mem_length,
                                                 int min_mem_length,
                                                 int reseed_length) {
    
#ifdef debug_mapper
#pragma omp critical
    {
        cerr << "find_mems: sequence ";
        for (auto iter = seq_begin; iter != seq_end; iter++) {
            cerr << *iter;
        }
        cerr << ", max mem length " << max_mem_length << ", min mem length " <<
                min_mem_length << ", reseed length " << reseed_length << endl;
    }
#endif
    
    
    if (!gcsa) {
        cerr << "error:[vg::Mapper] a GCSA2 index is required to query MEMs" << endl;
        exit(1);
    }
    
    if (min_mem_length > reseed_length && reseed_length) {
        cerr << "error:[vg::Mapper] minimimum reseed length for MEMs cannot be less than minimum MEM length" << endl;
        exit(1);
    }
    vector<MaximalExactMatch> mems;
    vector<pair<MaximalExactMatch, vector<size_t> > > sub_mems;

    gcsa::range_type full_range = gcsa::range_type(0, gcsa->size() - 1);
    
    // an empty sequence matches the entire bwt
    if (seq_begin == seq_end) {
        mems.push_back(MaximalExactMatch(seq_begin, seq_end, full_range));
    }
    
    // find SMEMs using GCSA+LCP array
    // algorithm sketch:
    // set up a cursor pointing to the last position in the sequence
    // set up a structure to track our MEMs, and set it == "" and full range match
    // while our cursor is >= the beginning of the string
    //   try a step of backwards searching using LF mapping
    //   if our range goes to 0
    //       go back to the last non-empty range
    //       emit the MEM corresponding to this range
    //       start a new mem
    //           use the LCP array's parent function to cut off the end of the match
    //           (effectively, this steps up the suffix tree)
    //           and calculate the new end point using the LCP of the parent node
    // emit the final MEM, if we finished in a matching state
    
    // next position we will extend matches to
    string::const_iterator cursor = seq_end - 1;
    
    // range of the last iteration
    gcsa::range_type last_range = full_range;
    
    // the temporary MEM we'll build up in this process
    MaximalExactMatch match(cursor, seq_end, full_range);
    
    // did we move the cursor or the end of the match last iteration?
    bool prev_iter_jumped_lcp = false;
    
    // loop maintains invariant that match.range contains the hits for seq[cursor+1:match.end]
    while (cursor >= seq_begin) {
        // break the MEM on N; which for DNA we assume is non-informative
        // this *will* match many places in assemblies, but it isn't helpful
        if (*cursor == 'N') {
            match.begin = cursor + 1;
            
            size_t mem_length = match.length();
            
            if (mem_length >= min_mem_length) {
                mems.push_back(match);
                
#ifdef debug_mapper
#pragma omp critical
                {
                    vector<gcsa::node_type> locations;
                    gcsa->locate(match.range, locations);
                    cerr << "adding MEM " << match.sequence() << " at positions ";
                    for (auto nt : locations) {
                        cerr << make_pos_t(nt) << " ";
                    }
                    cerr << endl;
                }
#endif
            }
            
            match.end = cursor;
            match.range = full_range;
            --cursor;
            
            // are we reseeding?
            if (reseed_length && mem_length >= reseed_length) {
                if (fast_reseed) {
                    find_sub_mems_fast(mems,
                                       match.end,
                                       max(min_mem_length, (int) mem_length / 2),
                                       sub_mems);
                }
                else {
                    find_sub_mems(mems,
                                  match.end,
                                  min_mem_length,
                                  sub_mems);
                }
            }
            
            prev_iter_jumped_lcp = false;
            
            // skip looking for matches since they are non-informative
            continue;
        }
        
        // hold onto our previous range
        last_range = match.range;
        
        // execute one step of LF mapping
        match.range = gcsa->LF(match.range, gcsa->alpha.char2comp[*cursor]);
        
        if (gcsa::Range::empty(match.range)
            || (max_mem_length && match.end - cursor > max_mem_length)
            || match.end - cursor > gcsa->order()) {
            
            // we've exhausted our BWT range, so the last match range was maximal
            // or: we have exceeded the order of the graph (FPs if we go further)
            // or: we have run over our parameter-defined MEM limit
            
            if (cursor + 1 == match.end) {
                // avoid getting caught in infinite loop when a single character mismatches
                // entire index (b/c then advancing the LCP doesn't move the search forward
                // at all, need to move the cursor instead)
                match.begin = cursor + 1;
                match.range = last_range;
                
                if (match.end - match.begin >= min_mem_length) {
                    mems.push_back(match);
                }
                
                match.end = cursor;
                match.range = full_range;
                --cursor;
                
                // don't reseed in empty MEMs
                
                prev_iter_jumped_lcp = false;
            }
            else {
                match.begin = cursor + 1;
                match.range = last_range;
                size_t mem_length = match.end - match.begin;
                
                // record the last MEM, but check to make sure were not actually still searching
                // for the end of the next MEM
                if (mem_length >= min_mem_length && !prev_iter_jumped_lcp) {
                    mems.push_back(match);
                    
#ifdef debug_mapper
#pragma omp critical
                    {
                        vector<gcsa::node_type> locations;
                        gcsa->locate(match.range, locations);
                        cerr << "adding MEM " << match.sequence() << " at positions ";
                        for (auto nt : locations) {
                            cerr << make_pos_t(nt) << " ";
                        }
                        cerr << endl;
                    }
#endif
                }
                
                // get the parent suffix tree node corresponding to the parent of the last MEM's STNode
                gcsa::STNode parent = lcp->parent(last_range);
                // set the MEM to be the longest prefix that is shared with another MEM
                match.end = match.begin + parent.lcp();
                // and set up the next MEM using the parent node range
                match.range = parent.range();
                
                // are we reseeding?
                if (reseed_length && mem_length >= reseed_length && !prev_iter_jumped_lcp) {
                    if (fast_reseed) {
                        find_sub_mems_fast(mems,
                                           match.end,
                                           max(min_mem_length, (int) mem_length / 2),
                                           sub_mems);
                    }
                    else {
                        find_sub_mems(mems,
                                      match.end,
                                      min_mem_length,
                                      sub_mems);
                    }
                }
                
                
                prev_iter_jumped_lcp = true;
            }
        }
        else {
            prev_iter_jumped_lcp = false;
            
            // just step to the next position
            --cursor;
        }
    }
    // TODO: is this where the bug with the duplicated MEMs is occurring? (when the prefix of a read
    // contains multiple non SMEM hits so that the iteration will loop through the LCP routine multiple
    // times before escaping out of the loop?
    
    // if we have a MEM at the beginning of the read, record it
    match.begin = seq_begin;
    size_t mem_length = match.end - match.begin;
    if (mem_length >= min_mem_length) {
        mems.push_back(match);
        
#ifdef debug_mapper
#pragma omp critical
        {
            vector<gcsa::node_type> locations;
            gcsa->locate(match.range, locations);
            cerr << "adding MEM " << match.sequence() << " at positions ";
            for (auto nt : locations) {
                cerr << make_pos_t(nt) << " ";
            }
            cerr << endl;
        }
#endif
        
        // are we reseeding?
        if (reseed_length && mem_length >= reseed_length) {
            if (fast_reseed) {
                find_sub_mems_fast(mems,
                                   match.begin,
                                   max(min_mem_length, (int) mem_length / 2),
                                   sub_mems);
            }
            else {
                find_sub_mems(mems,
                              match.begin,
                              min_mem_length,
                              sub_mems);
            }
        }
    }
    
    // fill the MEMs' node lists
    for (MaximalExactMatch& mem : mems) {
        mem.match_count = gcsa->count(mem.range);
        // if we aren't filtering on hit count, or if we have up to the max allowed hits
        if (mem.match_count > 0 && (!hit_max || mem.match_count <= hit_max)) {
            // extract the graph positions matching the range
            gcsa->locate(mem.range, mem.nodes);
        }
    }
    
    if (reseed_length) {
        // determine counts of matches
        for (pair<MaximalExactMatch, vector<size_t> >& sub_mem_and_parents : sub_mems) {
            // count in entire range, including parents
            sub_mem_and_parents.first.match_count = gcsa->count(sub_mem_and_parents.first.range);
            // remove parents from count
            for (size_t parent_idx : sub_mem_and_parents.second) {
                sub_mem_and_parents.first.match_count -= mems[parent_idx].match_count;
            }
        }

        // fill MEMs with positions
        for (auto& m : sub_mems) {
            auto& mem = m.first;
            if (mem.match_count > 0 && (!hit_max || mem.match_count <= hit_max)) {
                gcsa->locate(mem.range, mem.nodes);
            }
        }

        // combine the MEM and sub-MEM lists
        for (auto iter = sub_mems.begin(); iter != sub_mems.end(); iter++) {
            mems.push_back(std::move((*iter).first));
        }
        
    }
    
    // return the MEMs in order along the read
    // TODO: there should actually be a linear time method to merge and order the sub-MEMs, since
    // they are ordered by the parent MEMs
    std::sort(mems.begin(), mems.end(), [](const MaximalExactMatch& m1, const MaximalExactMatch& m2) {
        return m1.begin < m2.begin ? true : (m1.begin == m2.begin ? m1.end < m2.end : false);
    });
    
    // verify the matches (super costly at scale)
    /*
#ifdef debug_mapper
    if (debug) { check_mems(mems); }
#endif
    */
    return mems;
}
    
void Mapper::find_sub_mems(vector<MaximalExactMatch>& mems,
                           string::const_iterator next_mem_end,
                           int min_mem_length,
                           vector<pair<MaximalExactMatch, vector<size_t>>>& sub_mems_out) {
    
    // get the most recently added MEM
    MaximalExactMatch& mem = mems.back();
    
#ifdef debug_mapper
#pragma omp critical
    {
        cerr << "find_sub_mems: sequence ";
        for (auto iter = mem.begin; iter != mem.end; iter++) {
            cerr << *iter;
        }
        cerr << ", min mem length " << min_mem_length << endl;
    }
#endif
    
    // how many times does the parent MEM occur in the index?
    size_t parent_count = gcsa->count(mem.range);
    
    // next position where we will look for a match
    string::const_iterator cursor = mem.end - 1;
    
    // the righthand end of the sub-MEM we are building
    string::const_iterator sub_mem_end = mem.end;
    
    // the range that matches search_start:sub_mem_end
    gcsa::range_type range = gcsa::range_type(0, gcsa->size() - 1);
    
    // did we move the cursor or the end of the match last iteration?
    bool prev_iter_jumped_lcp = false;
    
    // look for matches that are contained in this MEM and not contained in the next MEM
    while (cursor >= mem.begin && sub_mem_end > next_mem_end) {
        // Note: there should be no need to handle N's or whole-index mismatches in this
        // routine (unlike the SMEM routine) since they should never make it into a parent
        // SMEM in the first place
        
        // hold onto our previous range
        gcsa::range_type last_range = range;
        // execute one step of LF mapping
        range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);

        if (gcsa->count(range) <= parent_count) {
            // there are no more hits outside of parent MEM hits, record the previous
            // interval as a sub MEM
            string::const_iterator sub_mem_begin = cursor + 1;

            if (sub_mem_end - sub_mem_begin >= min_mem_length && !prev_iter_jumped_lcp) {
                sub_mems_out.push_back(make_pair(MaximalExactMatch(sub_mem_begin, sub_mem_end, last_range),
                                                 vector<size_t>{mems.size() - 1}));
#ifdef debug_mapper
#pragma omp critical
                {
                    vector<gcsa::node_type> locations;
                    gcsa->locate(last_range, locations);
                    cerr << "adding sub-MEM ";
                    for (auto iter = sub_mem_begin; iter != sub_mem_end; iter++) {
                        cerr << *iter;
                    }
                    cerr << " at positions ";
                    for (auto nt : locations) {
                        cerr << make_pos_t(nt) << " ";
                    }
                    cerr << endl;
                    
                }
#endif
                // identify all previous MEMs that also contain this sub-MEM
                for (int64_t i = ((int64_t) mems.size()) - 2; i >= 0; i--) {
                    if (sub_mem_begin >= mems[i].begin) {
                        // contined in next MEM, add its index to sub MEM's list of parents
                        sub_mems_out.back().second.push_back(i);
                    }
                    else {
                        // not contained in the next MEM, cannot be contained in earlier ones
                        break;
                    }
                }
            }
#ifdef debug_mapper
            else {
#pragma omp critical
                {
                    cerr << "minimally more frequent MEM is too short ";
                    for (auto iter = sub_mem_begin; iter != sub_mem_end; iter++) {
                        cerr << *iter;
                    }
                    cerr << endl;
                }
            }
#endif
            
            // get the parent suffix tree node corresponding to the parent of the last MEM's STNode
            gcsa::STNode parent = lcp->parent(last_range);
            // set the MEM to be the longest prefix that is shared with another MEM
            sub_mem_end = sub_mem_begin + parent.lcp();
            // and get the next range as parent node range
            range = parent.range();
            
            prev_iter_jumped_lcp = true;
        }
        else {
            cursor--;
            prev_iter_jumped_lcp = false;
        }
    }
    
    // add a final sub MEM if there is one and it is not contained in the next parent MEM
    if (sub_mem_end > next_mem_end && sub_mem_end - mem.begin >= min_mem_length && !prev_iter_jumped_lcp) {
        sub_mems_out.push_back(make_pair(MaximalExactMatch(mem.begin, sub_mem_end, range),
                                         vector<size_t>{mems.size() - 1}));
#ifdef debug_mapper
#pragma omp critical
        {
            cerr << "adding sub-MEM ";
            for (auto iter = mem.begin; iter != sub_mem_end; iter++) {
                cerr << *iter;
            }
            cerr << endl;
        }
#endif
        // note: this sub MEM is at the far left side of the parent MEM, so we don't need to
        // check whether earlier MEMs contain it as well
    }
#ifdef debug_mapper
    else {
#pragma omp critical
        {
            cerr << "minimally more frequent MEM is too short ";
            for (auto iter = mem.begin; iter != sub_mem_end; iter++) {
                cerr << *iter;
            }
            cerr << endl;
        }
    }
#endif
}
    
void Mapper::find_sub_mems_fast(vector<MaximalExactMatch>& mems,
                                string::const_iterator next_mem_end,
                                int min_sub_mem_length,
                                vector<pair<MaximalExactMatch, vector<size_t>>>& sub_mems_out) {
    
#ifdef debug_mapper
#pragma omp critical
    cerr << "find_sub_mems_fast: mem ";
    for (auto iter = mems.back().begin; iter != mems.back().end; iter++) {
        cerr << *iter;
    }
    cerr << ", min_sub_mem_length " << min_sub_mem_length << endl;
#endif
    
    // get the most recently added MEM
    MaximalExactMatch& mem = mems.back();
    
    // how many times does the parent MEM occur in the index?
    size_t parent_count = gcsa->count(mem.range);
    
    // the end of the leftmost substring that is at least the minimum length and not contained
    // in the next SMEM
    string::const_iterator probe_string_end = mem.begin + min_sub_mem_length;
    if (probe_string_end <= next_mem_end) {
        probe_string_end = next_mem_end + 1;
    }
    
    while (probe_string_end <= mem.end) {
        
        // locate the probe substring of length equal to the minimum length for a sub-MEM
        // that we are going to test to see if it's inside any sub-MEM
        string::const_iterator probe_string_begin = probe_string_end - min_sub_mem_length;
        
#ifdef debug_mapper
#pragma omp critical
        cerr << "probe string is mem[" << probe_string_begin - mem.begin << ":" << probe_string_end - mem.begin << "] ";
        for (auto iter = probe_string_begin; iter != probe_string_end; iter++) {
            cerr << *iter;
        }
        cerr << endl;
#endif
        
        // set up LF searching
        string::const_iterator cursor = probe_string_end - 1;
        gcsa::range_type range = gcsa::range_type(0, gcsa->size() - 1);
        
        // check if the probe substring is more frequent than the SMEM its contained in
        bool probe_string_more_frequent = true;
        while (cursor >= probe_string_begin) {
            
            range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);
            
            if (gcsa->count(range) <= parent_count) {
                probe_string_more_frequent = false;
                break;
            }
            
            cursor--;
        }
        
        if (probe_string_more_frequent) {
            // this is the prefix of a sub-MEM of length >= the minimum, now we need to
            // find its end using binary search
            
            if (probe_string_end == next_mem_end + 1) {
                // edge case: we arbitrarily moved the probe string to the right to avoid finding
                // sub-MEMs that are contained in the next SMEM, so we don't have the normal guarantee
                // that this match cannot be extended to the left
                // to re-establish this guarantee, we need to walk it out as far as possible before
                // looking for the right end of the sub-MEM
                
                // extend match until beginning of SMEM or until the end of the independent hit
                while (cursor >= mem.begin) {
                    gcsa::range_type last_range = range;
                    range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);
                    
                    if (gcsa->count(range) <= parent_count) {
                        range = last_range;
                        break;
                    }
                    
                    cursor--;
                }
                
                // mark this position as the beginning of the probe substring
                probe_string_begin = cursor + 1;
            }
            
            // inclusive interval that contains the past-the-last index of the sub-MEM
            string::const_iterator left_search_bound = probe_string_end;
            string::const_iterator right_search_bound = mem.end;
            
            // the match range of the longest prefix of the sub-MEM we've found so far (if we initialize
            // it here, the binary search is guaranteed to LF along the full sub-MEM in some iteration)
            gcsa::range_type sub_mem_range = range;
            
            // iterate until inteveral contains only one index
            while (right_search_bound > left_search_bound) {
                
                string::const_iterator middle = left_search_bound + (right_search_bound - left_search_bound + 1) / 2;
                
#ifdef debug_mapper
#pragma omp critical
                cerr << "checking extension mem[" << probe_string_begin - mem.begin << ":" << middle - mem.begin << "] ";
                for (auto iter = probe_string_begin; iter != middle; iter++) {
                    cerr << *iter;
                }
                cerr << endl;
#endif
                
                // set up LF searching
                cursor = middle - 1;
                range = gcsa::range_type(0, gcsa->size() - 1);
                
                // check if there is an independent occurrence of this substring outside of the SMEM
                // TODO: potential optimization: if the range of matches at some index is equal to the
                // range of matches in an already confirmed independent match at the same index, then
                // it will still be so for the rest of the LF queries, so we can bail out of the loop
                // early as a match
                bool contained_in_independent_match = true;
                while (cursor >= probe_string_begin) {
                    
                    range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);
                    
                    if (gcsa->count(range) <= parent_count) {
                        contained_in_independent_match = false;
                        break;
                    }
                    
                    cursor--;
                }
                
                if (contained_in_independent_match) {
                    // the end of the sub-MEM must be here or to the right
                    left_search_bound = middle;
                    // update the range of matches (this is the longest match we've verified so far)
                    sub_mem_range = range;
                }
                else {
                    // the end of the sub-MEM must be to the left
                    right_search_bound = middle - 1;
                }
            }

#ifdef debug_mapper
#pragma omp critical
            cerr << "final sub-MEM is mem[" << probe_string_begin - mem.begin << ":" << right_search_bound - mem.begin << "] ";
            for (auto iter = probe_string_begin; iter != right_search_bound; iter++) {
                cerr << *iter;
            }
            cerr << endl;
#endif
            
            // record the sub-MEM
            sub_mems_out.push_back(make_pair(MaximalExactMatch(probe_string_begin, right_search_bound, sub_mem_range),
                                             vector<size_t>{mems.size() - 1}));

            // identify all previous MEMs that also contain this sub-MEM
            for (int64_t i = ((int64_t) mems.size()) - 2; i >= 0; i--) {
                if (probe_string_begin >= mems[i].begin) {
                    // contined in next MEM, add its index to sub MEM's list of parents
                    sub_mems_out.back().second.push_back(i);
                }
                else {
                    // not contained in the next MEM, cannot be contained in earlier ones
                    break;
                }
            }
            
            // the closest possible independent probe string will now occur one position
            // to the right of this sub-MEM
            probe_string_end = right_search_bound + 1;
            
        }
        else {
            // we've found a suffix of the probe substring that is only contained inside the
            // parent SMEM, so we can move far enough to the right that the next probe substring
            // will not contain it
            
            probe_string_end = cursor + min_sub_mem_length + 1;
        }
    }
}
    
void Mapper::first_hit_positions_by_index(MaximalExactMatch& mem,
                                          vector<set<pos_t>>& positions_by_index_out) {
    // find the hit to the first index in the parent MEM's range
    vector<gcsa::node_type> all_first_hits;
    gcsa->locate(mem.range.first, all_first_hits, true, false);
    
    // find where in the graph the first hit of the parent MEM is at each index
    mem_positions_by_index(mem, make_pos_t(all_first_hits[0]), positions_by_index_out);
    
    // in case the first hit occurs in more than one place, accumulate all the hits
    if (all_first_hits.size() > 1) {
        for (size_t i = 1; i < all_first_hits.size(); i++) {
            vector<set<pos_t>> temp_positions_by_index;
            mem_positions_by_index(mem, make_pos_t(all_first_hits[i]),
                                   temp_positions_by_index);
            
            for (size_t i = 0; i < positions_by_index_out.size(); i++) {
                for (const pos_t& pos : temp_positions_by_index[i]) {
                    positions_by_index_out[i].insert(pos);
                }
            }
        }
    }
}

void Mapper::fill_nonredundant_sub_mem_nodes(vector<MaximalExactMatch>& parent_mems,
                                             vector<pair<MaximalExactMatch, vector<size_t> > >::iterator sub_mem_records_begin,
                                             vector<pair<MaximalExactMatch, vector<size_t> > >::iterator sub_mem_records_end) {

    
    // for each MEM, a vector of the positions that it touches at each index along the MEM
    vector<vector<set<pos_t>>> positions_by_index(parent_mems.size());
    
    for (auto iter = sub_mem_records_begin; iter != sub_mem_records_end; iter++) {
        
        pair<MaximalExactMatch, vector<size_t> >& sub_mem_and_parents = *iter;
        
        MaximalExactMatch& sub_mem = sub_mem_and_parents.first;
        vector<size_t>& parent_idxs = sub_mem_and_parents.second;
        
        // how many total hits does each parent MEM have?
        vector<size_t> num_parent_hits;
        // positions their first hits of the parent MEM takes at the start position of the sub-MEM
        vector<set<pos_t>*> first_parent_mem_hit_positions;
        for (size_t parent_idx : parent_idxs) {
            // get the parent MEM
            MaximalExactMatch& parent_mem = parent_mems[parent_idx];
            num_parent_hits.push_back(gcsa->count(parent_mem.range));
            
            if (positions_by_index[parent_idx].empty()) {
                // the parent MEM's positions by index haven't been calculated yet, so do it

                first_hit_positions_by_index(parent_mem, positions_by_index[parent_idx]);

            }
            // the index along the parent MEM that sub MEM starts
            size_t offset = sub_mem.begin - parent_mem.begin;
            first_parent_mem_hit_positions.push_back(&(positions_by_index[parent_idx][offset]));
        }
        
        for (gcsa::size_type i = sub_mem.range.first; i <= sub_mem.range.second; i++) {
            
            // add the locations of the hits, but do not remove duplicates yet
            vector<gcsa::node_type> hits;
            gcsa->locate(i, hits, true, false);
            
            // the number of subsequent hits (including these) that are inside a parent MEM
            size_t parent_hit_jump = 0;
            for (gcsa::node_type node : hits) {
                // look for the hit in each parent MEM
                for (size_t j = 0; j < first_parent_mem_hit_positions.size(); j++) {
                    if (first_parent_mem_hit_positions[j]->count(make_pos_t(node))) {
                        // this hit is also a node on a path of the first occurrence of the parent MEM
                        // that means that this is the first index of the sub-range that corresponds
                        // to the parent MEM's hits
                        
                        // calculate how many more positions to jump
                        parent_hit_jump = num_parent_hits[j];
                        break;
                    }
                }
            }
            
            if (parent_hit_jump > 0) {
                // we're at the start of an interval of parent hits, skip the rest of it
                i += (parent_hit_jump - 1);
            }
            else {
                // these are nonredundant sub MEM hits, add them
                for (gcsa::node_type node : hits) {
                    sub_mem.nodes.push_back(node);
                }
            }
        }
        
        // remove duplicates (copied this functionality from the gcsa locate function, but
        // I don't actually know what it's purpose is)
        gcsa::removeDuplicates(sub_mem.nodes, false);
    }
}

void Mapper::mem_positions_by_index(MaximalExactMatch& mem, pos_t hit_pos,
                                    vector<set<pos_t>>& positions_by_index_out) {
    
    // this is a specialized DFS that keeps track of both the distance along the MEM
    // and the position(s) in the graph in the stack by adding all of the next reachable
    // positions in a layer (i.e. vector) in the stack at the end of each iteration.
    // it also keeps track of whether a position in the graph matched to a position along
    // the MEM can potentially be extended to the full MEM to avoid combinatorially checking
    // all paths through bubbles
    
    size_t mem_length = std::distance(mem.begin, mem.end);
    
    // indicates a pairing of this graph position and this MEM index could be extended to a full match
    positions_by_index_out.clear();
    positions_by_index_out.resize(mem_length);
    
    // indicates a pairing of this graph position and this MEM index could not be extended to a full match
    vector<set<pos_t>> false_pos_by_mem_index(mem_length);
    
    // each record indicates the next edge index to traverse, the number of edges that
    // cannot reach a MEM end, and the positions along each edge out
    vector<pair<pair<size_t, size_t>, vector<pos_t> > > pos_stack;
    pos_stack.push_back(make_pair(make_pair((size_t) 0 , (size_t) 0), vector<pos_t>{hit_pos}));
    
    while (!pos_stack.empty()) {
        size_t mem_idx = pos_stack.size() - 1;

        // which edge are we going to search out of this node next?
        size_t next_idx = pos_stack.back().first.first;
        
        if (next_idx >= pos_stack.back().second.size()) {
            // we have traversed all of the edges out of this position
            
            size_t num_misses = pos_stack.back().first.second;
            bool no_full_matches_possible = (num_misses == pos_stack.back().second.size());
            
            // backtrack to previous node
            pos_stack.pop_back();

            // if necessary, mark the edge into this node as a miss
            if (no_full_matches_possible && !pos_stack.empty()) {
                // all of the edges out failed to reach the end of a MEM, this position is a dead end
                
                // get the position that traversed into the layer we just popped off
                pos_t prev_graph_pos = pos_stack.back().second[pos_stack.back().first.first - 1];
                
                // unlabel this node as a potential hit and instead mark it as a miss
                positions_by_index_out[mem_idx].erase(prev_graph_pos);
                false_pos_by_mem_index[mem_idx].insert(prev_graph_pos);
                
                // increase the count of misses in this layer
                pos_stack.back().first.second++;
            }
            
            // skip the forward search on this iteration
            continue;
        }
        
        // increment to the next edge
        pos_stack.back().first.first++;
        
        pos_t graph_pos = pos_stack.back().second[next_idx];
        
        
        // did we already find a MEM through this position?
        if (positions_by_index_out[mem_idx].count(graph_pos)) {
            // we don't need to check the same MEM suffix again
            continue;
        }
        
        // did we already determine that you can't reach a MEM through this position?
        if (false_pos_by_mem_index[mem_idx].count(graph_pos)) {
            // increase the count of misses in this layer
            pos_stack.back().first.second++;
            
            // we don't need to check the same MEM suffix again
            continue;
        }
        
        // does this graph position match the MEM?
        if (*(mem.begin + mem_idx) != xg_cached_pos_char(graph_pos, xindex, get_node_cache())) {
            // mark this node as a miss
            false_pos_by_mem_index[mem_idx].insert(graph_pos);
            
            // increase the count of misses in this layer
            pos_stack.back().first.second++;
        }
        else {
            // mark this node as a potential hit
            positions_by_index_out[mem_idx].insert(graph_pos);
            
            // are we finished with the MEM?
            if (mem_idx < mem_length - 1) {
                
                // add a layer onto the stack for all of the edges out
                pos_stack.push_back(make_pair(make_pair((size_t) 0 , (size_t) 0),
                                              vector<pos_t>()));
                
                // fill the layer with the next positions
                vector<pos_t>& nexts = pos_stack.back().second;
                for (const pos_t& next_graph_pos : positions_bp_from(graph_pos, 1, false)) {
                    nexts.push_back(next_graph_pos);
                }
            }
        }
    }
}

void Mapper::check_mems(const vector<MaximalExactMatch>& mems) {
    for (auto mem : mems) {
#ifdef debug_mapper
#pragma omp critical
        cerr << "checking MEM: " << mem.sequence() << endl;
#endif
        // TODO: fix this for sub-MEMs
        if (sequence_positions(mem.sequence()) != gcsa_nodes_to_positions(mem.nodes)) {
            cerr << "SMEM failed! " << mem.sequence()
                 << " expected " << sequence_positions(mem.sequence()).size() << " hits "
                 << "but found " << gcsa_nodes_to_positions(mem.nodes).size()
                 << "(aside: this consistency check is broken for sub-MEMs, oops)" << endl;
        }
    }
}

const string mems_to_json(const vector<MaximalExactMatch>& mems) {
    stringstream s;
    s << "[";
    size_t j = 0;
    for (auto& mem : mems) {
        s << "[\"";
        s << mem.sequence();
        s << "\",[";
        size_t i = 0;
        for (auto& node : mem.nodes) {
            s << "\"" << gcsa::Node::decode(node) << "\"";
            if (++i < mem.nodes.size()) s << ",";
        }
        s << "]]";
        if (++j < mems.size()) s << ",";
    }
    s << "]";
    return s.str();
}

char Mapper::pos_char(pos_t pos) {
    return xg_cached_pos_char(pos, xindex, get_node_cache());
}

map<pos_t, char> Mapper::next_pos_chars(pos_t pos) {
    return xg_cached_next_pos_chars(pos, xindex, get_node_cache(), get_edge_cache());
}

int Mapper::graph_distance(pos_t pos1, pos_t pos2, int maximum) {
    return xg_cached_distance(pos1, pos2, maximum, xindex, get_node_cache(), get_edge_cache());
}

int Mapper::approx_position(pos_t pos) {
    // get nodes on the forward strand
    if (is_rev(pos)) {
        pos = reverse(pos, xg_cached_node_length(id(pos), xindex, get_node_cache()));
    }
    return xg_cached_node_start(id(pos), xindex, get_node_start_cache()) + offset(pos);
}

int Mapper::approx_distance(pos_t pos1, pos_t pos2) {
    return approx_position(pos1) - approx_position(pos2);
}

/// returns approximate position of alignnment start in xindex
/// or -1.0 if alignment is unmapped
int Mapper::approx_alignment_position(const Alignment& aln) {
    if (aln.path().mapping_size()) {
        auto& mbeg = aln.path().mapping(0);
        if (mbeg.has_position()) {
            return approx_position(make_pos_t(mbeg.position()));
        } else {
            return -1.0;
        }
    } else {
        return -1.0;
    }
}

/// returns approximate distance between alignment starts
/// or -1.0 if not possible to determine
int Mapper::approx_fragment_length(const Alignment& aln1, const Alignment& aln2) {
    int pos1 = approx_alignment_position(aln1);
    int pos2 = approx_alignment_position(aln2);
    if (pos1 != -1 && pos2 != -1) {
        return abs(pos1 - pos2);
    } else {
        return -1;
    }
}

id_t Mapper::node_approximately_at(int approx_pos) {
    return xindex->node_at_seq_pos(
        min(xindex->seq_length,
            (size_t)max(approx_pos, 1)));
}

set<pos_t> Mapper::positions_bp_from(pos_t pos, int distance, bool rev) {
    return xg_cached_positions_bp_from(pos, distance, rev, xindex, get_node_cache(), get_edge_cache());
}

// use LRU caching to get the most-recent node positions
map<string, vector<size_t> > Mapper::node_positions_in_paths(gcsa::node_type node) {
    auto& pos_cache = get_node_pos_cache();
    auto cached = pos_cache.retrieve(node);
    if(!cached.second) {
        // todo use approximate estimate
        cached.first = xindex->position_in_paths(gcsa::Node::id(node), gcsa::Node::rc(node), gcsa::Node::offset(node));
        pos_cache.put(node, cached.first);
    }
    return cached.first;
}

Alignment Mapper::walk_match(const string& seq, pos_t pos) {
    //cerr << "in walk match with " << seq << " " << seq.size() << " " << pos << endl;
    Alignment aln;
    aln.set_sequence(seq);
    auto alns = walk_match(aln, seq, pos);
    if (!alns.size()) {
        //cerr << "no alignments returned from walk match with " << seq << " " << seq.size() << " " << pos << endl;
        //assert(false);
        return aln;
    }
    aln = alns.front(); // take the first one we found
    //assert(alignment_to_length(aln) == alignment_from_length(aln));
    if (alignment_to_length(aln) != alignment_from_length(aln)
        || alignment_to_length(aln) != seq.size()) {
        //cerr << alignment_to_length(aln) << " is not " << seq.size() << endl;
        //cerr << pb2json(aln) << endl;
        //assert(false);
        aln.clear_path();
    }
#ifdef debug_mapper
    if (debug) {
        cerr << "walk_match result " << pb2json(aln) << endl;
        if (!check_alignment(aln)) {
            cerr << "aln is invalid!" << endl;
            exit(1);
        }
    }
#endif
    return aln;
}

vector<Alignment> Mapper::walk_match(const Alignment& base, const string& seq, pos_t pos) {
    //cerr << "in walk_match " << seq << " from " << pos << " with base " << pb2json(base) << endl;
    // go to the position in the xg index
    // and step in the direction given
    // until we exhaust our sequence
    // or hit another node
    vector<Alignment> alns;
    Alignment aln = base;
    Path& path = *aln.mutable_path();
    Mapping* mapping = path.add_mapping();
    *mapping->mutable_position() = make_position(pos);
#ifdef debug_mapper
#pragma omp critical
    if (debug) cerr << "walking match for seq " << seq << " at position " << pb2json(*mapping) << endl;
#endif
    // get the first node we match
    int total = 0;
    size_t match_len = 0;
    for (size_t i = 0; i < seq.size(); ++i) {
        char c = seq[i];
        //cerr << string(base.path().mapping_size(), ' ') << pos << " @ " << i << " on " << c << endl;
        auto nexts = next_pos_chars(pos);
        // we can have a match on the current node
        if (nexts.size() == 1 && id(nexts.begin()->first) == id(pos)) {
            pos_t npos = nexts.begin()->first;
            // check that the next position would match
            if (i+1 < seq.size()) {
                // we can't step, so we break
                //cerr << "Checking if " << pos_char(npos) << " != " << seq[i+1] << endl;
                if (pos_char(npos) != seq[i+1]) {
#ifdef debug_mapper
#pragma omp critical
                    if (debug) cerr << "MEM does not match position, returning without creating alignment" << endl;
#endif
                    return alns;
                }
            }
            // otherwise we step our counters
            ++match_len;
            ++get_offset(pos);
        } else { // or we go into the next node
            // we must be going into another node
            // emit the mapping for this node
            //cerr << "we are going into a new node" << endl;
            // finish the last node
            {
                // we must have matched / we already checked
                ++match_len;
                Edit* edit = mapping->add_edit();
                edit->set_from_length(match_len);
                edit->set_to_length(match_len);
                // reset our counter
                match_len = 0;
            }
            // find the next node that matches our MEM
            bool got_match = false;
            if (i+1 < seq.size()) {
                //cerr << "nexts @ " << i << " " << nexts.size() << endl;
                for (auto& p : nexts) {
                    //cerr << " next : " << p.first << " " << p.second << " (looking for " << seq[i+1] << ")" << endl;
                    if (p.second == seq[i+1]) {
                        if (!got_match) {
                            pos = p.first;
                            got_match = true;
                        } else {
                            auto v = walk_match(aln, seq.substr(i+1), p.first);
                            if (v.size()) {
                                alns.reserve(alns.size() + distance(v.begin(), v.end()));
                                alns.insert(alns.end(), v.begin(), v.end());
                            }
                        }
                    }
                }
                if (!got_match) {
                    // this matching ends here
                    // and we haven't finished matching
                    // thus this path doesn't contain the match
                    //cerr << "got no match" << endl;
                    return alns;
                }

                // set up a new mapping
                mapping = path.add_mapping();
                *mapping->mutable_position() = make_position(pos);
            } else {
                //cerr << "done!" << endl;
            }
        }
    }
    if (match_len) {
        Edit* edit = mapping->add_edit();
        edit->set_from_length(match_len);
        edit->set_to_length(match_len);
    }
    alns.push_back(aln);
#ifdef debug_mapper
#pragma omp critical
    if (debug) {
        cerr << "walked alignment(s):" << endl;
        for (auto& aln : alns) {
            cerr << pb2json(aln) << endl;
        }
    }
#endif
    //cerr << "returning " << alns.size() << endl;
    return alns;
}

// convert one mem into a set of alignments, one for each exact match
vector<Alignment> Mapper::mem_to_alignments(MaximalExactMatch& mem) {
    vector<Alignment> alns;
    const string seq = mem.sequence();
    for (auto& node : mem.nodes) {
        pos_t pos = make_pos_t(node);
        alns.emplace_back(walk_match(seq, pos));
    }
    return alns;
}

Alignment Mapper::patch_alignment(const Alignment& aln) {
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "patching " << pb2json(aln) << endl;
            if (!check_alignment(aln)) {
                cerr << "aln is invalid!" << endl;
                exit(1);
            }
        }
    }
#endif
    Alignment patched;
    int score = 0;
    // walk along the alignment and find the portions that are unaligned
    int read_pos = 0;
    auto& path = aln.path();
    auto aligner = get_regular_aligner();
    auto qual_adj_aligner = get_qual_adj_aligner();
    for (int i = 0; i < path.mapping_size(); ++i) {
        auto& mapping = path.mapping(i);
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "looking at mapping " << pb2json(mapping) << endl;
        }
#endif
        pos_t ref_pos = make_pos_t(mapping.position());
        Mapping* new_mapping = patched.mutable_path()->add_mapping();
        *new_mapping->mutable_position() = mapping.position();
        for (int j = 0; j < mapping.edit_size(); ++j) {
            auto& edit = mapping.edit(j);
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "looking at edit " << pb2json(edit) << endl;
            }
#endif
            if (edit_is_match(edit)) {
                // matches behave as expected
                //cerr << "edit is match" << endl;
                if (!aln.quality().empty() && adjust_alignments_for_base_quality) {
                    //cerr << read_pos << " " << edit.to_length() << endl;
                    score += qual_adj_aligner->score_exact_match(
                        aln.sequence().substr(read_pos, edit.to_length()),
                        aln.quality().substr(read_pos, edit.to_length()));
                } else {
                    score += edit.from_length()*aligner->match;
                }
                *new_mapping->add_edit() = edit;
            } else if (edit_is_deletion(edit)) {
                // we can't do anything for deletions-- anyway they shouldn't get here if we call this
                // in the SMEM threading alignment
                score -= aligner->gap_open + edit.from_length()*aligner->gap_extension;
                *new_mapping->add_edit() = edit;
            } else if (edit_is_insertion(edit)) {
                //cerr << "looking at " << edit.sequence() << endl;
                // bits to patch in are recorded like insertions
                // pick up the graph from the start to end where we have an unaligned bit
                // but bail out if we get a lot of graph
                bool go_forward = !is_rev(ref_pos);
                bool go_backward = is_rev(ref_pos);
                id_t id1 = id(ref_pos);
                id_t id2 = 0;
                pos_t after_pos = ref_pos;
                bool soft_clip_to_left = false;
                bool soft_clip_to_right = false;
                // this is a soft clip
                if (i == 0 && j == 0) {
                    //cerr << "first soft clip" << endl;
                    // todo we should flip the orientation of the soft clip flag around if we are reversed
                    // ...
                    //soft_clip_on_start = true;
                    if (is_rev(ref_pos)) {
                        soft_clip_to_right = true;
                        go_forward = true;
                        go_backward = false;
                    } else {
                        soft_clip_to_left = true;
                        go_forward = false;
                        go_backward = true;
                    }
                } else if (j+1 < mapping.edit_size()) {
                    id2 = id1;
                    //cerr << "more edits to go on this node: " << id2 << " " << after_pos << endl;
                } else if (i+1 < path.mapping().size()) {
                    // get the next position in the partial alignment we're patching
                    id2 = path.mapping(i+1).position().node_id();
                    after_pos = make_pos_t(path.mapping(i+1).position());
                    //cerr << "we're up so look for id: " << id2 << " " << after_pos << endl;
                } else {
                    //cerr << "last soft clip" << endl;
                    if (is_rev(ref_pos)) {
                        soft_clip_to_left = true;
                        go_forward = false;
                        go_backward = true;
                    } else {
                        soft_clip_to_right = true;
                        go_forward = true;
                        go_backward = false;
                    }
                }
                //cerr << "working from " << ref_pos << endl;
                // only go backward if we are at the first edit (e.g. soft clip)
                // otherwise we go forward

                // find the cut positions (on the forward strand)
                // if they are on the same node, use the multi-cut interface
                // if they are on separate, cut each node
                // todo... update the multi-cut interface to produce a translation
                // or alternatively, write the translation here for prototyping
                // generate the translation for the rest of the graph
                // delete any head or tail bits that we shouldn't be able to align to
                // instantiate a translator object (make sure it can handle the deleted bits?)
                // translate the alignment

                pos_t first_cut = ref_pos;
                pos_t second_cut = ref_pos;
                bool insertion_between_mems = false;
                if (j+1 < mapping.edit_size()) {
                    //cerr << "not the last edit" << endl;
                    if (edit.from_length()) {
                        get_offset(second_cut) += edit.from_length();
                    } else if (i != 0 && j != 0) {
                        insertion_between_mems = true;
                    }
                } else if (i+1 < path.mapping_size()) {
                    //cerr << "not the last mapping" << endl;
                    // we have to look at the next mapping
                    second_cut = make_pos_t(path.mapping(i+1).position());
                } else {
                    //cerr << "end of alignment" << endl;
                    // nothing to do
                }
                pos_t next_pos = second_cut;

                //cerr << "first_cut before " << first_cut << endl;
                //cerr << "second_cut before " << second_cut << endl;

                // if we get a target graph
                int min_distance = edit.to_length() * 3;

                //cerr << "going at least " << min_distance << endl;
                VG graph;
                if (!insertion_between_mems) {
                    xindex->get_id_range(id1, id1, graph.graph);
                    xindex->expand_context(graph.graph,
                                           min_distance,
                                           false, // don't use steps (use length)
                                           false, // don't add paths
                                           go_forward,
                                           go_backward,
                                           id2);  // our target node
                    graph.rebuild_indexes();
#ifdef debug_mapper
#pragma omp critical
                    {
                        if (debug) cerr << "got graph " << graph.size() << " " << pb2json(graph.graph) << endl;
                    }
#endif
                    //graph.serialize_to_file("raw-" + hash_alignment(aln) + ".vg");
                }

                // we have to remember how much we've trimmed from the first node
                // so that we can translate it after the fact
                map<id_t, pair<int, int> > trimmings;
                vector<id_t> target_nodes;

                // TODO continue if the graph doesn't have both cut points
                if (insertion_between_mems || !graph.has_node(id(first_cut)) || !graph.has_node(id(second_cut))) {
                    // treat the bit as unalignable
#ifdef debug_mapper
#pragma omp critical
                    {
                        if (debug) cerr << "graph does not contain both cut points!" << endl;
                    }
#endif
                } else {

                    // now trim the graph to fit by cutting the head/tail node(s)
                    bool align_rc = false;
                    if (is_rev(first_cut) && is_rev(second_cut)) {
                        pos_t tmp_cut = first_cut;
                        first_cut = reverse(second_cut, graph.get_node(id(second_cut))->sequence().size());
                        second_cut = reverse(tmp_cut, graph.get_node(id(tmp_cut))->sequence().size());
                        align_rc = true;
                    } else {
                        if (is_rev(first_cut)) {
                            reverse(first_cut, graph.get_node(id(first_cut))->sequence().size());
                        }
                        if (is_rev(second_cut)) {
                            first_cut = reverse(second_cut, graph.get_node(id(second_cut))->sequence().size());
                        }
                    }

                    //cerr << "first_cut after " << first_cut << endl;
                    //cerr << "second_cut after " << second_cut << endl;

                    if (id(first_cut) == id(second_cut)) {
                        if (offset(first_cut) == offset(second_cut)) {
                            bool begin_cut = !offset(first_cut);
                            bool end_cut = (offset(first_cut) == graph.get_node(id(first_cut))->sequence().size());
                            if (!begin_cut && !end_cut) {
                                //cerr << "cut has offset" << endl;
                                Node* left; Node* right; Node* trimmed;
                                Node* node = graph.get_node(id(first_cut));
                                graph.divide_node(node, offset(first_cut), left, right);
                                //cerr << pb2json(*left) << " | " << pb2json(*right) << endl;
                                // check soft clip status, which will change what part we keep
                                // keep the part that's relevant to the soft clip resolution
                                if (soft_clip_to_left) {
                                    //cerr << "soft clip to left" << endl;
                                    graph.destroy_node(right);
                                    graph.swap_node_id(left, id(first_cut));
                                    trimmed = left;
                                    trimmings[id(first_cut)] = make_pair(0, offset(first_cut));
                                } else {
                                    //cerr << "soft clip to right or other" << endl;
                                    graph.destroy_node(left);
                                    graph.swap_node_id(right, id(first_cut));
                                    trimmed = right;
                                    trimmings[id(first_cut)] = make_pair(offset(first_cut), 0);
                                }
                                if (trimmed->sequence().size()) {
                                    target_nodes.push_back(trimmed->id());
                                } else {
                                    // push back each connected node
                                    for (auto& edge : graph.edges_to(trimmed)) {
                                        target_nodes.push_back(edge->from());
                                    }
                                    for (auto& edge : graph.edges_from(trimmed)) {
                                        target_nodes.push_back(edge->to());
                                    }
                                }
                            } else {
                                // erase everything before this node
                                // do so by removing edges
                                // later we will decide which subgraphs to keep
                                // check soft clip status, which will change what part we keep
                                if (soft_clip_to_left) {
                                    NodeSide keep = NodeSide(id(first_cut), false);
                                    for (auto& side : graph.sides_to(keep)) {
                                        target_nodes.push_back(side.node);
                                        graph.destroy_edge(side, keep);
                                    }
                                } else if (soft_clip_to_right) {
                                    NodeSide keep = NodeSide(id(first_cut), true);
                                    for (auto& side : graph.sides_from(keep)) {
                                        target_nodes.push_back(side.node);
                                        graph.destroy_edge(keep, side);
                                    }
                                } else {
                                    if (begin_cut) {
                                        assert(false);
                                    }
                                    if (end_cut) {
                                        assert(false);
                                    }
                                }
                            }
                        } else {
                            //cerr << "offsets different same node" << endl;
                            vector<int> positions = { (int)offset(first_cut), (int)offset(second_cut) };
                            vector<Node*> parts;
                            Node* node = graph.get_node(id(first_cut));
                            size_t orig_len = node->sequence().size();
                            graph.divide_node(node, positions, parts);
                            // now remove the end parts
                            graph.destroy_node(parts.front());
                            graph.destroy_node(parts.back());
                            graph.swap_node_id(parts.at(1), id(first_cut));
                            target_nodes.push_back(id(first_cut));
                            trimmings[id(first_cut)] = make_pair(offset(first_cut),
                                                                 orig_len - offset(second_cut));
                        }
                    } else { // different nodes to trim
                        //cerr << "different nodes" << endl;
                        if (offset(first_cut)) {
                            Node* left; Node* right;
                            Node* node = graph.get_node(id(first_cut));
                            graph.divide_node(node, offset(first_cut), left, right);
                            // remove the unused part
                            graph.destroy_node(left);
                            graph.swap_node_id(right, id(first_cut));
                            //target_nodes.push_back(graph.get_node(id(first_cut)));
                            Node* trimmed = graph.get_node(id(first_cut));
                            trimmings[id(first_cut)] = make_pair(offset(first_cut),
                                                                 0);
                            if (trimmed->sequence().size()) {
                                target_nodes.push_back(trimmed->id());
                            } else {
                                // push back each connected node
                                for (auto& edge : graph.edges_to(trimmed)) {
                                    target_nodes.push_back(edge->from());
                                }
                                for (auto& edge : graph.edges_from(trimmed)) {
                                    target_nodes.push_back(edge->to());
                                }
                            }
                        } else {
                            // destroy everything ahead of the node
                            NodeSide begin = NodeSide(id(first_cut));
                            for (auto& side : graph.sides_to(begin)) {
                                graph.destroy_edge(side, begin);
                            }
                            target_nodes.push_back(id(first_cut));
                        }

                        if (offset(second_cut)) {
                            Node* left; Node* right;
                            Node* node = graph.get_node(id(second_cut));
                            graph.divide_node(node, offset(second_cut), left, right);
                            // remove the unused part
                            graph.destroy_node(right);
                            graph.swap_node_id(left, id(second_cut));
                            //target_nodes.push_back(graph.get_node(id(second_cut)));
                            Node* trimmed = graph.get_node(id(first_cut));
                            if (trimmed->sequence().size()) {
                                target_nodes.push_back(trimmed->id());
                            } else {
                                // push back each connected node
                                for (auto& edge : graph.edges_to(trimmed)) {
                                    target_nodes.push_back(edge->from());
                                }
                                for (auto& edge : graph.edges_from(trimmed)) {
                                    target_nodes.push_back(edge->to());
                                }
                            }
                        } else {
                            // but we need to record the things in the graph connected to it
                            for (auto& side : graph.sides_to(id(second_cut))) {
                                target_nodes.push_back(side.node);
                            }
                            // destroy the node
                            graph.destroy_node(id(second_cut));
                            // we don't record this node as a target as we've destroyed it
                        }
                    }
                    graph.remove_null_nodes_forwarding_edges();
                    graph.remove_orphan_edges();
                }
                // reselect the target subgraph
                VG target;
                for (auto& id : target_nodes) {
                    if (graph.has_node(id)) {
                        target.add_node(*graph.get_node(id));
                    }
                }
                graph.expand_context(target, edit.sequence().size(), false);
                graph = target;
                // now do the alignment
                if (graph.empty()) {
#ifdef debug_mapper
#pragma omp critical
                    {
                        if (debug) {
                            cerr << "no target for alignment of " << edit.sequence()
                                 << ", graph is empty" << endl;
                        }
                    }
#endif
                    score -= aligner->gap_open + edit.to_length()*aligner->gap_extension;
                    *new_mapping->add_edit() = edit;
                } else {
                    // we've set the graph to the trimmed target
#ifdef debug_mapper
#pragma omp critical
                    {
                        if (debug) cerr << "target graph " << graph.size() << " " << pb2json(graph.graph) << endl;
                    }
#endif
                    //time to try an alignment
                    Alignment patch;
                    bool flip = mapping.position().is_reverse();
                    if (flip) {
                        patch.set_sequence(reverse_complement(edit.sequence()));
                        if (!aln.quality().empty()) {
                            string qual = aln.quality().substr(read_pos, edit.to_length());
                            reverse(qual.begin(), qual.end());
                            patch.set_quality(qual);
                        }
                    } else {
                        patch.set_sequence(edit.sequence());
                        if (!aln.quality().empty()) {
                            patch.set_quality(aln.quality().substr(read_pos, edit.to_length()));
                        }
                    }

                    // do the alignment
                    bool banded_global = !soft_clip_to_right && !soft_clip_to_left;
                    bool pinned_alignment = soft_clip_to_right || soft_clip_to_left;
                    bool pinned_reverse = false;
                    if (soft_clip_to_right) {
                        pinned_reverse = true;
                    }

                    patch = align_to_graph(patch,
                                           graph,
                                           max_query_graph_ratio,
                                           pinned_alignment,
                                           pinned_reverse,
                                           full_length_alignment_bonus,
                                           banded_global);

                    // adjust the translated node positions
                    for (int k = 0; k < patch.path().mapping_size(); ++k) {
                        auto* mapping = patch.mutable_path()->mutable_mapping(k);
                        auto t = trimmings.find(mapping->position().node_id());
                        if (t != trimmings.end()) {
                            auto trimmed_length_fwd = t->second.first;
                            auto trimmed_length_rev = t->second.second;
                            mapping->mutable_position()->set_offset(
                                mapping->position().offset() +
                                ( mapping->position().is_reverse() ? trimmed_length_rev : trimmed_length_fwd ));
                        }
                    }

                    // reverse complement back if we've flipped the read for alignment
                    if (flip) {
                        patch = reverse_complement_alignment(patch,
                                                             (function<int64_t(int64_t)>) ([&](int64_t id) {
                                                                     return (int64_t)get_node_length(id);
                                                                 }));
                    }

                    if (debug && !check_alignment(patch)) {
                        cerr << "patching failure " << pb2json(patched) << endl;
                        assert(false);
                    }

                    // append the chunk to patched

#ifdef debug_mapper
#pragma omp critical
                    {
                        if (debug) cerr << "patch: " << pb2json(patch) << endl;
                    }
#endif
                    patch.clear_sequence(); // we set the whole sequence later
                    if (!patch.path().mapping_size() || min_identity && patch.identity() < min_identity) {
                        //cerr << "doing that other thing" << endl;
                        score -= aligner->gap_open + edit.to_length()*aligner->gap_extension;
                        *new_mapping->add_edit() = edit;
                    } else {
                        //cerr << "extending alignment" << endl;
                        auto last_mapping = patched.mutable_path()->mutable_mapping(patched.path().mapping_size()-1);
                        if (last_mapping->edit_size() == 0
                            && last_mapping->position().node_id() != 0) {
                            // if we just did an alignment, use its position rather than a previous hint
                            // such as for soft clips
                            patched = merge_alignments(patch, patched, false);
                        } else {
                            extend_alignment(patched, patch, true);

                        }
                        // point at the correct "new mapping"
                        new_mapping = patched.mutable_path()->mutable_mapping(patched.path().mapping_size()-1);
                        score += patch.score();
                    }
                    //cerr << "extended " << pb2json(patched) << endl;
                }
            }
            // update our offsets
            get_offset(ref_pos) += edit.from_length();
            read_pos += edit.to_length();
        }
        //cerr << "growing patched: " << pb2json(patched) << endl;
        /*
        #ifdef debug_mapper
    if (debug) {
            patched.set_sequence(aln.sequence().substr(0, read_pos));
            if (!check_alignment(patched)) {
                cerr << "patched failure " << pb2json(patched) << endl;
                assert(false);
            }
        }
        */
    }
    // finally, fix up the alignment score
    patched.set_sequence(aln.sequence());
    if (!aln.quality().empty()) {
        patched.set_quality(aln.quality());
    }
    // simplify the mapping representation
    patched = simplify(patched);
    // optionally smooth with realignment
    if (smooth_alignments) {
        patched = smooth_alignment(patched);
    }
    // set the identity
    patched.set_identity(identity(patched.path()));
    // recompute the score
    patched.set_score(score_alignment(patched));
    return patched;
}

Alignment Mapper::smooth_alignment(const Alignment& aln) {
    // find cases where we have reversals
    auto& path = aln.path();
    Alignment head, tail, smoothed;
    bool should_smooth = false;
    for (int i = 0; i < path.mapping_size(); ++i) {
        auto& mapping = path.mapping(i);
        size_t to_len = mapping_to_length(mapping);
        size_t from_len = mapping_to_length(mapping);
        if (mapping_to_length(mapping) != mapping_from_length(mapping)) {
            should_smooth = true;
            break;
        }
        // two mappings to the same node
        if (i < path.mapping_size()-1
            && path.mapping(i).position().node_id()
            == path.mapping(i+1).position().node_id()) {
            should_smooth = true;
            break;
        }
    }
    if (should_smooth) {
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "smoothing " << pb2json(aln) << endl;
            }
        }
#endif
        // get the subgraph overlapping the alignment
        VG graph;
        int count_fwd = 0;
        int count_rev = 0;
        for (int i = 0; i < aln.path().mapping_size(); ++i) {
            auto& mapping = aln.path().mapping(i);
            if (mapping.has_position() && mapping.position().node_id()) {
                if (mapping.position().is_reverse()) {
                    ++count_rev;
                } else {
                    ++count_fwd;
                }
                if (mapping.position().node_id()) {
                    graph.add_node(xindex->node(mapping.position().node_id()));
                }
            }
        }
        xindex->expand_context(graph.graph, 1, false);
        graph.rebuild_indexes();
        // re-do the alignment
        // against the graph
        // always use the banded global mode
        smoothed = aln;
        // take a majority opinion about our orientation
        bool flip = count_rev > count_fwd;
        if (flip) {
            smoothed.set_sequence(reverse_complement(aln.sequence()));
            if (!aln.quality().empty()) {
                string qual = aln.quality();
                reverse(qual.begin(), qual.end());
                smoothed.set_quality(qual);
            }
        }
        bool banded_global = false;
        bool pinned_alignment = false;
        bool pinned_reverse = false;
        smoothed = align_to_graph(smoothed,
                                  graph,
                                  max_query_graph_ratio,
                                  pinned_alignment,
                                  pinned_reverse,
                                  full_length_alignment_bonus,
                                  banded_global);
        if (flip) {
            smoothed = reverse_complement_alignment(smoothed,
                                                    (function<int64_t(int64_t)>) ([&](int64_t id) {
                                                            return (int64_t)get_node_length(id);
                                                        }));
        }
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "smoothed " << pb2json(smoothed) << endl;
            }
        }
#endif
        return simplify(smoothed);
    } else {
        return aln;
    }
}

// generate a score from the alignment without realigning
// handles split alignments, where gaps of unknown length are
// by estimating length using the positional paths embedded in the graph
int32_t Mapper::score_alignment(const Alignment& aln) {
    int score = 0;
    int read_offset = 0;
    auto& path = aln.path();
    auto aligner = get_regular_aligner();
    auto qual_adj_aligner = get_qual_adj_aligner();
    for (int i = 0; i < path.mapping_size(); ++i) {
        auto& mapping = path.mapping(i);
        //cerr << "looking at mapping " << pb2json(mapping) << endl;
        for (int j = 0; j < mapping.edit_size(); ++j) {
            auto& edit = mapping.edit(j);
            //cerr << "looking at edit " << pb2json(edit) << endl;
            if (edit_is_match(edit)) {
                if (!aln.quality().empty() && adjust_alignments_for_base_quality) {
                    score += qual_adj_aligner->score_exact_match(
                        aln.sequence().substr(read_offset, edit.to_length()),
                        aln.quality().substr(read_offset, edit.to_length()));
                } else {
                    score += edit.from_length()*aligner->match;
                }
            } else if (edit_is_sub(edit)) {
                score -= aligner->mismatch * edit.sequence().size();
            } else if (edit_is_deletion(edit)) {
                score -= aligner->gap_open + edit.from_length()*aligner->gap_extension;
            } else if (edit_is_insertion(edit)
                       && !((i == 0 && j == 0)
                            || (i == path.mapping_size()-1
                                && j == mapping.edit_size()-1))) {
                // todo how do we score this qual adjusted?
                score -= aligner->gap_open + edit.to_length()*aligner->gap_extension;
            }
            read_offset += edit.to_length();
        }
        // score any intervening gaps in mappings using approximate distances
        if (i+1 < path.mapping_size()) {
            // what is the distance between the last position of this mapping
            // and the first of the next
            Position last_pos = mapping.position();
            last_pos.set_offset(last_pos.offset() + mapping_from_length(mapping));
            Position next_pos = path.mapping(i+1).position();
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "gap: " << make_pos_t(last_pos) << " to " << make_pos_t(next_pos) << endl;
            }
#endif
            int dist = graph_distance(make_pos_t(last_pos), make_pos_t(next_pos), aln.sequence().size());
            if (dist == aln.sequence().size()) {
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "could not find distance to next target, using approximation" << endl;
                }
#endif
                dist = abs(approx_distance(make_pos_t(last_pos), make_pos_t(next_pos)));
            }
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "distance from " << pb2json(last_pos) << " to " << pb2json(next_pos) << " is " << dist << endl;
            }
#endif
            if (dist > 0) {
                score -= aligner->gap_open + dist * aligner->gap_extension;
            }
        }
    }
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "score from score_alignment " << score << endl;
    }
#endif
    return max(0, score);
}

int32_t Mapper::rescore_without_full_length_bonus(const Alignment& aln) {
    int32_t score = aln.score();
    if (softclip_start(aln) == 0) {
        score -= full_length_alignment_bonus;
    }
    if (softclip_end(aln) == 0) {
        score -= full_length_alignment_bonus;
    }
    return score;
}

// make a perfect-match alignment out of a vector of MEMs which each have only one recorded hit
// use the base alignment sequence (which the SMEMs relate to) to fill in the gaps
Alignment Mapper::mems_to_alignment(const Alignment& aln, vector<MaximalExactMatch>& mems) {
    // base case--- empty alignment
    if (mems.empty()) {
        Alignment aln; return aln;
    }
    vector<Alignment> alns;
    // get reference to the start and end of the sequences
    string::const_iterator seq_begin = aln.sequence().begin();
    string::const_iterator seq_end = aln.sequence().end();
    // we use this to track where we need to add sequence
    string::const_iterator last_end = seq_begin;
    for (int i = 0; i < mems.size(); ++i) {
        auto& mem = mems.at(i);
        //cerr << "looking at " << mem.sequence() << endl;
        // this mem is contained in the last
        if (mem.end <= last_end) {
            continue;
        }
        // handle unaligned portion between here and the last SMEM or start of read
        if (mem.begin > last_end) {
            alns.emplace_back();
            alns.back().set_sequence(aln.sequence().substr(last_end - seq_begin, mem.begin - last_end));
        }
        Alignment aln = mem_to_alignment(mem);
        // find and trim overlap with previous
        if (i > 0) {
            // use the end of the last mem we touched (we may have skipped several)
            int overlap = last_end - mem.begin;
            if (overlap > 0) {
                aln = strip_from_start(aln, overlap);
            }
        }
        alns.push_back(aln);
        last_end = mem.end;
    }
    // handle unaligned portion at end of read
    int start = last_end - seq_begin;
    int length = seq_end - (seq_begin + start);
    
    alns.emplace_back();
    alns.back().set_sequence(aln.sequence().substr(start, length));

    auto alnm = merge_alignments(alns);
    *alnm.mutable_quality() = aln.quality();
    return alnm;
}

// convert one mem into an alignment; validates that only one node is given
Alignment Mapper::mem_to_alignment(MaximalExactMatch& mem) {
    const string seq = mem.sequence();
    if (mem.nodes.size() > 1) {
        cerr << "[vg::Mapper] warning: generating first alignment from MEM with multiple recorded hits" << endl;
    }
    auto& node = mem.nodes.front();
    pos_t pos = make_pos_t(node);
    return walk_match(seq, pos);
}

vector<Alignment> Mapper::align_mem_multi(const Alignment& alignment, vector<MaximalExactMatch>& mems, double& cluster_mq, int additional_multimaps) {

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "aligning " << pb2json(alignment) << endl;
    }
#endif
    if (!gcsa || !xindex) {
        cerr << "error:[vg::Mapper] a GCSA2/xg index pair is required for MEM mapping" << endl;
        exit(1);
    }

    if (mem_chaining) {
        return mems_pos_clusters_to_alignments(alignment, mems, additional_multimaps, cluster_mq);
    } else {
        return mems_id_clusters_to_alignments(alignment, mems, additional_multimaps);
    }

}

vector<Alignment>
Mapper::mems_id_clusters_to_alignments(const Alignment& alignment, vector<MaximalExactMatch>& mems, int additional_multimaps) {

    struct StrandCounts {
        uint32_t forward;
        uint32_t reverse;
    };
    
    int total_multimaps = max_multimaps + additional_multimaps;

    // we will use these to determine the alignment strand for each subgraph
    map<id_t, StrandCounts> node_strands;
    // records a mapping of id->MEMs, for cluster ranking
    map<id_t, vector<MaximalExactMatch*> > id_to_mems;
    // for clustering
    vector<id_t> ids;

    // run through the mems, generating a set of alignments for each
    for (auto& mem : mems) {
        //#ifdef debug_mapper
        //if (debug) cerr << "on mem " << mem.sequence() << endl;
        //#endif
        size_t len = mem.begin - mem.end;
        // collect ids and orientations of hits to them on the forward mem
        for (auto& node : mem.nodes) {
            id_t id = gcsa::Node::id(node);
            id_to_mems[id].push_back(&mem);
            ids.push_back(id);
            if (gcsa::Node::rc(node)) {
                node_strands[id].reverse++;
            } else {
                node_strands[id].forward++;
            }
        }
    }

    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    // establish clusters using approximate distance metric based on ids
    // we pick up ranges between successive nodes
    // when these are below our thread_extension length
    vector<vector<id_t> > clusters;
    for (auto& id : ids) {
        if (clusters.empty()) {
            clusters.emplace_back();
            auto& l = clusters.back();
            l.push_back(id);
        } else {
            auto& prev = clusters.back().back();
            if (id - prev <= thread_extension) {
                clusters.back().push_back(id);
            } else {
                clusters.emplace_back();
                auto& l = clusters.back();
                l.push_back(id);
            }
        }
    }

    // rank the clusters by the fraction of the read that they cover
    map<vector<id_t>*, int> cluster_query_coverage;
    std::for_each(clusters.begin(), clusters.end(),
                  [&cluster_query_coverage,
                   &id_to_mems](vector<id_t>& cluster) {
                      set<string::const_iterator> query_coverage;
                      for (auto& id : cluster) {
                          auto& mems = id_to_mems[id];
                          std::for_each(mems.begin(), mems.end(),
                                        [&](MaximalExactMatch* m) {
                                            string::const_iterator c = m->begin;
                                            while (c != m->end) query_coverage.insert(c++);
                                        });
                      }
                      cluster_query_coverage[&cluster] = query_coverage.size();
                  });

    vector<vector<id_t>*> ranked_clusters;
    std::for_each(clusters.begin(), clusters.end(),
                  [&ranked_clusters](vector<id_t>& cluster) {
                      ranked_clusters.push_back(&cluster); });

    std::sort(ranked_clusters.begin(), ranked_clusters.end(),
              [&cluster_query_coverage](vector<id_t>* a,
                                        vector<id_t>* b) {
                  auto len_a = cluster_query_coverage[a];
                  auto len_b = cluster_query_coverage[b];
                  // order by cluster coverage of query
                  // break ties on number of MEMs (fewer better)
                  if (len_a == len_b) {
                      return a->size() < b->size();
                  } else {
                      return len_a > len_b;
                  }
              });


    // generate an alignment for each subgraph/orientation combination for which we have hits
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "aligning to " << clusters.size() << " clusters" << endl;
            for (auto cptr : ranked_clusters) {
                auto& c = *cptr;
                cerr << cluster_query_coverage[cptr] << ":"
                     << c.size() << " "
                     << c.front() << "-" << c.back() << endl;
            }
        }
    }
#endif

    vector<Alignment> alns; // our alignments
    
    // set up our forward and reverse base alignments (these are just sequences in bare alignment objs)
    auto aln_fw = alignment;
    aln_fw.clear_path();
    aln_fw.set_score(0);
    auto aln_rc = reverse_complement_alignment(aln_fw, (function<int64_t(int64_t)>)
                                               ([&](int64_t id) { return get_node_length(id); }));
    
    int max_target_length = alignment.sequence().size() * max_target_factor;

    size_t attempts = 0;
    for (auto& cptr : ranked_clusters) {
        auto& cluster = *cptr;
        // skip if our cluster is too small
        if (cluster.size() < cluster_min) continue;
        // record our attempt count
        ++attempts;
        // bail out if we've passed our maximum number of attempts
        if (attempts > max(max_attempts, total_multimaps)) break;
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "attempt " << attempts
                     << " on cluster " << cluster.front() << "-" << cluster.back() << endl;
            }
        }
#endif
        VG sub; // the subgraph we'll align against
        set<id_t> seen;
        for (auto& id : cluster) {
            if (seen.count(id)) continue; // avoid double-gets
            seen.insert(id);
            xindex->get_id_range(id, id, sub.graph);
        }
        // expand using our context depth
        xindex->expand_context(sub.graph, context_depth, false);
        sub.rebuild_indexes();
        // if the graph is now too big to attempt, bail out
        if (max_target_factor && sub.length() > max_target_length) continue;
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "attempt " << attempts
                     << " on subgraph " << sub.min_node_id() << "-" << sub.max_node_id() << endl;
            }
        }
#endif
        // determine the likely orientation
        uint32_t fw_mems = 0;
        uint32_t rc_mems = 0;
        sub.for_each_node([&](Node* n) {
                auto ns = node_strands.find(n->id());
                if (ns != node_strands.end()) {
                    fw_mems += ns->second.forward;
                    rc_mems += ns->second.reverse;
                }
            });
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "got " << fw_mems << " forward and " << rc_mems << " reverse mems" << endl;
        }
#endif
        if (fw_mems) {
            Alignment aln = align_to_graph(aln_fw, sub, max_query_graph_ratio);
            resolve_softclips(aln, sub);
            alns.push_back(aln);
            if (attempts >= total_multimaps &&
                greedy_accept &&
                aln.identity() >= accept_identity) {
                break;
            }
        }
        if (rc_mems) {
            Alignment aln = align_to_graph(aln_rc, sub, max_query_graph_ratio);
            resolve_softclips(aln, sub);
            alns.push_back(reverse_complement_alignment(aln,
                                                        (function<int64_t(int64_t)>)
                                                        ([&](int64_t id) { return get_node_length(id); })));
            if (attempts >= total_multimaps &&
                greedy_accept &&
                aln.identity() >= accept_identity) {
                break;
            }
        }
    }
    
    return alns;
}

void Mapper::resolve_softclips(Alignment& aln, VG& graph) {

    if (!xindex) {
        cerr << "error:[vg::Mapper] xg index pair is required for dynamic softclip resolution" << endl;
        exit(1);
    }
    // we can't resolve softclips on a read without a mapping
    if (!aln.path().mapping_size()) return;
    // we can be more precise about our handling of softclips due to the low cost
    // of the fully in-memory xg index
    int sc_start = softclip_start(aln);
    int sc_end = softclip_end(aln);
    int last_score = aln.score();
    size_t itr = 0;
    Path* path = aln.mutable_path();
    int64_t idf = path->mutable_mapping(0)->position().node_id();
    int64_t idl = path->mutable_mapping(path->mapping_size()-1)->position().node_id();
    int max_target_length = aln.sequence().size() * max_target_factor;
    while (itr++ < max_softclip_iterations
           && (sc_start > softclip_threshold
               || sc_end > softclip_threshold)) {
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "Softclip before expansion: " << sc_start << " " << sc_end
                    << " (" << aln.score() << " points)" << endl;
            }
        }
#endif
        double avg_node_size = graph.length() / (double)graph.size();
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "average node size " << avg_node_size << endl;
        }
#endif
        // step towards the side where there were soft clips
        Graph flanks;
        xindex->get_id_range(idf, idf, flanks);
        xindex->get_id_range(idl, idl, flanks);
        xindex->expand_context(flanks,
                               max(context_depth, (int)((sc_start+sc_end)/avg_node_size)),
                               false, // don't add paths
                               true); // use steps
        graph.extend(flanks);

        aln.clear_path();
        aln.set_score(0);

        // give up if the graph is too big
        if (max_target_factor && graph.length() >= max_target_length) break;

        // otherwise, align
        aln = align_to_graph(aln, graph, max_query_graph_ratio);

        sc_start = softclip_start(aln);
        sc_end = softclip_end(aln);
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "Softclip after expansion: " << sc_start << " " << sc_end
                    << " (" << aln.score() << " points)" << endl;
            }
        }
#endif
        // we are not improving, so increasing the window is unlikely to help
        if (last_score == aln.score()) break;
        // update tracking of path end
        last_score = aln.score();
        path = aln.mutable_path();
        idf = path->mutable_mapping(0)->position().node_id();
        idl = path->mutable_mapping(path->mapping_size()-1)->position().node_id();
    }
}

// core alignment algorithm that handles both kinds of sequence indexes
vector<Alignment> Mapper::align_threaded(const Alignment& alignment, int& kmer_count, int kmer_size, int stride, int attempt) {

    // parameters, some of which should probably be modifiable
    // TODO -- move to Mapper object

    if (index == nullptr && (xindex == nullptr || gcsa == nullptr)) {
        cerr << "error:[vg::Mapper] index(es) missing, cannot map alignment!" << endl;
        exit(1);
    }

    const string& sequence = alignment.sequence();

    // Generate all the kmers we want to look up, with the correct stride.
    auto kmers = balanced_kmers(sequence, kmer_size, stride);

    //vector<uint64_t> sizes;
    //index->approx_sizes_of_kmer_matches(kmers, sizes);

    // Holds the map from node ID to collection of start offsets, one per kmer we're searching for.
    vector<map<int64_t, vector<int32_t> > > positions(kmers.size());
    int i = 0;
    for (auto& k : kmers) {
        if (!allATGC(k)) continue; // we can't handle Ns in this scheme
        //#ifdef debug_mapper
        //if (debug) cerr << "kmer " << k << " entropy = " << entropy(k) << endl;
        if (min_kmer_entropy > 0 && entropy(k) < min_kmer_entropy) continue;

        // We fill this in only once if we're using GCSA indexing
        gcsa::range_type gcsa_range;

        // Work out the number of *bytes* of matches for this kmer with the appropriate index.
        uint64_t approx_matches;
        if(gcsa) {
            // A little more complicated. We run the search and count the range size
            gcsa_range = gcsa->find(k);
            // Measure count and convert to bytes
            approx_matches = gcsa::Range::length(gcsa_range) * sizeof(gcsa::node_type);
        } else if(index) {
           approx_matches = index->approx_size_of_kmer_matches(k);
        } else {
            cerr << "error:[vg::Mapper] no search index present" << endl;
            exit(1);
        }

        // Report the approximate match byte size
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << k << "\t~" << approx_matches << endl;
        }
#endif
        // if we have more than one block worth of kmers on disk, consider this kmer non-informative
        // we can do multiple mapping by rnelaxing this
        if (approx_matches > hit_size_threshold) {
            continue;
        }

        // Grab the map from node ID to kmer start positions for this particular kmer.
        auto& kmer_positions = positions.at(i);
        // Fill it in, since we know there won't be too many to work with.

        if(gcsa) {
            // We need to fill in this vector with the GCSA nodes and then convert.
            std::vector<gcsa::node_type> gcsa_nodes;
            gcsa->locate(gcsa_range, gcsa_nodes);

            for(gcsa::node_type gcsa_node : gcsa_nodes) {
                if(gcsa::Node::rc(gcsa_node)) {
                    // We found a kmer on the reverse strand. The old index
                    // didn't handle these, so we ignore them. TODO: figure out
                    // how to account for them.
                    continue;
                }
                // Decode the result's ID and offset and record it
                kmer_positions[gcsa::Node::id(gcsa_node)].push_back(gcsa::Node::offset(gcsa_node));
            }

        } else if(index) {
           index->get_kmer_positions(k, kmer_positions);
        } else {
            cerr << "error:[vg::Mapper] no search index present" << endl;
            exit(1);
        }


        // ignore this kmer if it has too many hits
        // typically this will be filtered out by the approximate matches filter
        if (kmer_positions.size() > hit_max) kmer_positions.clear();
        // Report the actual match count for the kmer
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "\t=" << kmer_positions.size() << endl;
        }
#endif
        kmer_count += kmer_positions.size();
        ++i;
    }

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "kept kmer hits " << kmer_count << endl;
    }
#endif

    // make threads
    // these start whenever we have a kmer match which is outside of
    // one of the last positions (for the previous kmer) + the kmer stride % wobble (hmm)

    // For each node ID, holds the numbers of the kmers that we find on it, in
    // the order that they appear in the query. One would expect them to be
    // monotonically increasing.
    map<int64_t, vector<int> > node_kmer_order;

    // Maps from node ID and offset to a thread ending with the kmer that starts
    // there, if any such thread exists.
    map<pair<int64_t, int32_t>, vector<int64_t> > position_threads;

    // For each node, holds the last thread for that node. Because we only do
    // position wobble, threads only touch a single node.
    map<int64_t, vector<int64_t> > node_threads;

    //int node_wobble = 0; // turned off...

    // How far left or right from the "correct" position for the previous kmer
    // are we willing to search when looking for a thread to extend?
    int position_wobble = 2;

    int max_iter = sequence.size();
    int iter = 0;
    int64_t max_subgraph_size = 0;

    // This is basically the index on this loop over kmers and their position maps coming up
    i = 0;
    for (auto& p : positions) {
        // For every map from node ID to collection of kmer starts, for kmer i...

        // Grab the kmer and advance i for next loop iteration
        auto& kmer = kmers.at(i++);
        for (auto& x : p) {
            // For each node ID and the offsets on that node at which this kmer appears...
            int64_t id = x.first;
            vector<int32_t>& pos = x.second;

            // Note that this kmer is the next kmer in the query to appear in that node.
            node_kmer_order[id].push_back(i-1);
            for (auto& y : pos) {
                // For every offset along the node at which this kmer appears, in order...

                //cerr << kmer << "\t" << i << "\t" << id << "\t" << y << endl;
                // thread rules
                // if we find the previous position

                // This holds the thread that this instance of this kmer on this node is involved in.
                vector<int64_t> thread;

                // If we can find a thread close enough to this kmer, we want to
                // continue it with this kmer. If nothing changed between the
                // query and the reference, we would expect to extend the thread
                // that has its last kmer starting exactly stride bases before
                // this kmer starts (i.e. at y - stride). However, due to indels
                // existing, we search with a "wobble" of up to position_wobble
                // in either direction, outwards from the center.

                // This holds the current wobble that we are searching (between
                // -position_wobble and +position_wobble).
                int m = 0;
                for (int j = 0; j < 2*position_wobble + 1; ++j) {
                    // For each of the 2 * position_wobble + 1 wobble values we
                    // need to try, calculate the jth wobble value out from the
                    // center.
                    if (j == 0) { // on point
                        // First we use the zero wobble, which we started with
                    } else if (j % 2 == 0) { // subtract
                        // Every even step except the first, we try the negative version of the positive wobble we just tried
                        m *= -1;
                    } else { // add
                        // Every odd step, we try the positive version of the
                        // negative (or 0) wobble we just tried, incremented by
                        // 1.
                        m *= -1; ++m;
                    }

                    //cerr << "checking " << id << " " << y << " - " << kmer_size << " + " << m << endl;

                    // See if we can find a thread at this wobbled position
                    auto previous = position_threads.find(make_pair(id, y - stride + m));
                    if (previous != position_threads.end()) {
                        // If we did find one, use it as our thread, remove it
                        // so it can't be extended by anything else, and stop
                        // searching more extreme wobbles.

                        //length = position_threads[make_pair(id, y - stride + m)] + 1;
                        thread = previous->second;
                        position_threads.erase(previous);
                        //cerr << "thread is " << thread.size() << " long" << endl;
                        break;
                    }
                }

                // Now we either have the thread we are extending in thread, or we are starting a new thread.

                // Extend the thread with another kmer on this node ID.
                thread.push_back(id);
                // Save the thread as ending with a kmer at this offset on this node.
                position_threads[make_pair(id, y)] = thread;

                // This is now the last thread for this node.
                node_threads[id] = thread;
            }
        }
    }

    // This maps from a thread length (in kmer instances) to all the threads of that length.
    map<int, vector<vector<int64_t> > > threads_by_length;
    for (auto& t : node_threads) {
        auto& thread = t.second;
        auto& threads = threads_by_length[thread.size()];
        threads.push_back(thread);
    }

    // now sort the threads and re-cluster them

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "initial threads" << endl;
            for (auto& t : threads_by_length) {
                auto& length = t.first;
                auto& threads = t.second;
                cerr << length << ":" << endl;
                for (auto& thread : threads) {
                    cerr << "\t";
                    for (auto& id : thread) {
                        cerr << id << " ";
                    }
                    cerr << endl;
                }
                cerr << endl;
            }
        }
    }
#endif

    // sort threads by ids, taking advantage of vector comparison and how sets work
    set<vector<int64_t> > sorted_threads;
    auto tl = threads_by_length.rbegin();
    for (auto& t : node_threads) {
        auto& thread = t.second;
        sorted_threads.insert(thread);
    }
    threads_by_length.clear();

    // go back through and combine closely-linked threads
    // ... but only if their kmer order is proper

    // This holds threads by the last node ID they touch.
    map<int64_t, vector<int64_t> > threads_by_last;

    // go from threads that are longer to ones that are shorter
    for (auto& thread : sorted_threads) {
        //cerr << thread.front() << "-" << thread.back() << endl;

        // Find the earliest-ending thread that ends within max_thread_gap nodes of this thread's start
        auto prev = threads_by_last.upper_bound(thread.front()-max_thread_gap);
        //if (prev != threads_by_last.begin()) --prev;
        // now we should be at the highest thread within the bounds
        //cerr << prev->first << " " << thread.front() << endl;
        // todo: it may also make sense to check that the kmer order makes sense
        // what does this mean? it means that the previous
        if (prev != threads_by_last.end()
            && prev->first > thread.front() - max_thread_gap) {
            // If we found such a thread, and it also *starts* within
            // max_thread_gap nodes of this thread's start, we want to add our
            // thread onto the end of it and keep only the combined longer
            // thread. TODO: this limits max thread length.
            vector<int64_t> new_thread;
            auto& prev_thread = prev->second;
            new_thread.reserve(prev_thread.size() + thread.size());
            new_thread.insert(new_thread.end(), prev_thread.begin(), prev_thread.end());
            new_thread.insert(new_thread.end(), thread.begin(), thread.end());
            threads_by_last.erase(prev);
            // this will clobber... not good
            // maybe overwrite only if longer?
            threads_by_last[new_thread.back()] = new_thread;
        } else {
            // We want to keep this thread since it couldn't attach to any other thread.
            threads_by_last[thread.back()] = thread;
        }
    }

    // debugging
    /*
    #ifdef debug_mapper
    if (debug) {
        cerr << "threads by last" << endl;
        for (auto& t : threads_by_last) {
            auto& thread = t.second;
            cerr << t.first << "\t";
            for (auto& id : thread) {
                cerr << id << " ";
            }
            cerr << endl;
        }
    }
    #endif
    */

    // rebuild our threads_by_length set
    for (auto& t : threads_by_last) {
        auto& thread = t.second;
        if (thread.size() >= cluster_min) {
            // Only keep threads if they have a sufficient number of kmer instances in them.
            auto& threads = threads_by_length[thread.size()];
            threads.push_back(thread);
        }
    }

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "threads ready for alignment" << endl;
            for (auto& t : threads_by_length) {
                auto& length = t.first;
                auto& threads = t.second;
                cerr << length << ":" << endl;
                for (auto& thread : threads) {
                    cerr << "\t";
                    for (auto& id : thread) {
                        cerr << id << " ";
                    }
                    cerr << endl;
                }
                cerr << endl;
            }
        }
    }
#endif

    int thread_ex = thread_extension;
    map<vector<int64_t>*, Alignment> alignments;
    int8_t match = alignment.quality().empty() ? get_regular_aligner()->match : get_qual_adj_aligner()->match;

    // collect the nodes from the best N threads by length
    // and expand subgraphs as before
    //cerr << "extending by " << thread_ex << endl;
    tl = threads_by_length.rbegin();
    bool accepted = false;
    for (int i = 0;
         !accepted
             && tl != threads_by_length.rend()
             && (best_clusters == 0 || i < best_clusters);
         ++i, ++tl) {
        auto& threads = tl->second;
        // by definition, our thread should construct a contiguous graph
        for (auto& thread : threads) {
            // Do an alignment to the subgraph for each thread.

            // thread extension should be determined during iteration
            // note that there is a problem and hits tend to be imbalanced
            // due to the fact that we record the node position of the start of the kmer
            int64_t first = max((int64_t)0, *thread.begin());
            int64_t last = *thread.rbegin() + thread_ex;
            // so we can pick it up efficiently from the index by pulling the range from first to last
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "getting node range " << first << "-" << last << endl;
            }
#endif
            VG* graph = new VG;

            // Now we need to get the neighborhood by ID and expand outward by actual
            // edges. How we do this depends on what indexing structures we have.
            // TODO: We're repeating this code. Break it out into a function or something.
            if(xindex) {
                xindex->get_id_range(first, last, graph->graph);
                xindex->expand_context(graph->graph, context_depth, false);
                graph->rebuild_indexes();
            } else if(index) {
                index->get_range(first, last, *graph);
                index->expand_context(*graph, context_depth);
            } else {
                cerr << "error:[vg::Mapper] cannot align mate with no graph data" << endl;
                exit(1);
            }

            Alignment& ta = alignments[&thread];
            ta = alignment;

            // by default, expand the graph a bit so we are likely to map
            //index->get_connected_nodes(*graph);
            graph->remove_orphan_edges();

#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "got subgraph with " << graph->node_count() << " nodes, "
                                << graph->edge_count() << " edges" << endl;
            }
#endif
            //serialize_to_file("init-" + alignment.sequence() + "-" + hash_alignment(alignment).substr(0,8) + "-" + hash().substr(0,8) + ".vg");

            // Topologically sort the graph, breaking cycles and orienting all edges end to start.
            // This flips some nodes around, so we need to translate alignments back.
            //set<int64_t> flipped_nodes;
            //graph->orient_nodes_forward(flipped_nodes);

            // align
            //graph->serialize_to_file("align2.vg");
            ta.clear_path();
            ta.set_score(0);

            ta = align_to_graph(ta, *graph, max_query_graph_ratio);

            // check if we start or end with soft clips
            // if so, try to expand the graph until we don't have any more (or we hit a threshold)
            // expand in the direction where there were soft clips

            if (!ta.has_path()) continue;

            // we can be more precise about our handling of softclips due to the low cost
            // of the fully in-memory xg index
            int sc_start = softclip_start(ta);
            int sc_end = softclip_end(ta);
            int last_score = ta.score();
            size_t itr = 0;
            Path* path = ta.mutable_path();
            int64_t idf = path->mutable_mapping(0)->position().node_id();
            int64_t idl = path->mutable_mapping(path->mapping_size()-1)->position().node_id();
            int32_t d_to_head = graph->distance_to_head(NodeTraversal(graph->get_node(idf), false), sc_start*3);
            int32_t d_to_tail = graph->distance_to_tail(NodeTraversal(graph->get_node(idl), false), sc_end*3);
            while (itr++ < 3
                   && ((sc_start > softclip_threshold
                        && d_to_head >= 0 && d_to_head < sc_start)
                       || (sc_end > softclip_threshold
                           && d_to_tail >=0 && d_to_tail < sc_end))) {
                           
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) {
                        cerr << "softclip before " << sc_start << " " << sc_end << endl;
                        cerr << "distance to head "
                             << graph->distance_to_head(NodeTraversal(graph->get_node(idf), false), sc_start*3)
                             << endl;
                        cerr << "distance to tail "
                             << graph->distance_to_tail(NodeTraversal(graph->get_node(idl), false), sc_end*3)
                             << endl;
                    }
                }
#endif
                double avg_node_size = graph->length() / (double)graph->size();
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "average node size " << avg_node_size << endl;
                }
#endif
                // step towards the side where there were soft clips
                if (sc_start) {
                    if (xindex) {
                        Graph flank;
                        xindex->get_id_range(idf-1, idf, flank);
                        xindex->expand_context(flank,
                                               max(context_depth, (int)(sc_start/avg_node_size)),
                                               false);
                        graph->extend(flank);
                    } else if (index) {
                        VG flank;
                        index->get_range(max((int64_t)0, idf-thread_ex), idf, flank);
                        index->expand_context(flank, context_depth);
                        graph->extend(flank);
                    }
                }
                if (sc_end) {
                    if (xindex) {
                        Graph flank;
                        xindex->get_id_range(idl, idl+1, flank);
                        xindex->expand_context(flank,
                                               max(context_depth, (int)(sc_end/avg_node_size)),
                                               false);
                        graph->extend(flank);
                    } else if (index) {
                        VG flank;
                        index->get_range(idl, idl+thread_ex, flank);
                        index->expand_context(flank, context_depth);
                        graph->extend(flank);
                    }
                }
                graph->remove_orphan_edges();
                ta.clear_path();
                ta.set_score(0);

                ta = align_to_graph(ta, *graph, max_query_graph_ratio);

                sc_start = softclip_start(ta);
                sc_end = softclip_end(ta);
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "softclip after " << sc_start << " " << sc_end << endl;
                }
#endif
                // we are not improving, so increasing the window is unlikely to help
                if (last_score == ta.score()) break;
                // update tracking of path end
                last_score = ta.score();
                path = ta.mutable_path();
                idf = path->mutable_mapping(0)->position().node_id();
                idl = path->mutable_mapping(path->mapping_size()-1)->position().node_id();
                d_to_head = graph->distance_to_head(NodeTraversal(graph->get_node(idf), false), sc_start*3);
                d_to_tail = graph->distance_to_tail(NodeTraversal(graph->get_node(idl), false), sc_end*3);
            }

            delete graph;

#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "normalized score is " << (float)ta.score() / ((float)ta.sequence().size()*match) << endl;
            }
#endif
            if (greedy_accept && ta.identity() >= accept_identity) {
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "greedy accept" << endl;
                }
#endif
                accepted = true;
                break;
            }
        }
    }
    
    vector<Alignment> alns;
    alns.reserve(alignments.size());
    for (auto& ta : alignments) {
        alns.push_back(ta.second);
    }
    return alns;

}


// transform the path into a path relative to another path (defined by path_name)
// source -> surjection (in path_name coordinate space)
// the product is equivalent to a pairwise alignment between this path and the other

// new approach
// get path sequence
// get graph component overlapping path
// removing elements which aren't in the path of interest
// realign to this graph
// cross fingers

Alignment Mapper::surject_alignment(const Alignment& source,
                                    set<string>& path_names,
                                    string& path_name,
                                    int64_t& path_pos,
                                    bool& path_reverse,
                                    int window) {

    Alignment surjection = source;
    surjection.clear_mapping_quality();
    surjection.clear_score();
    surjection.clear_identity();
    surjection.clear_path();

    // get start and end nodes in path
    // get range between +/- window
    if (!source.has_path() || source.path().mapping_size() == 0) {
#ifdef debug

#pragma omp critical (cerr)
        cerr << "Alignment " << source.name() << " is unmapped and cannot be surjected" << endl;

#endif
        return surjection;
    }

    set<id_t> nodes;
    for (int i = 0; i < source.path().mapping_size(); ++ i) {
        nodes.insert(source.path().mapping(i).position().node_id());
    }
    VG graph;
    for (auto& node : nodes) {
        *graph.graph.add_node() = xindex->node(node);
    }
    xindex->expand_context(graph.graph, context_depth, true); // get connected edges and path
    graph.paths.append(graph.graph);
    graph.rebuild_indexes();

    set<string> kept_paths;
    graph.keep_paths(path_names, kept_paths);

    // We need this for inverting mappings to the correct strand
    function<int64_t(id_t)> node_length = [&graph](id_t node) {
        return graph.get_node(node)->sequence().size();
    };
    
    // What is our alignment to surject spelled the other way around? We can't
    // just use the normal alignment RC function because the mappings reference
    // nonexistent nodes.
    // Make sure to copy all the things about the alignment (name, etc.)

    Alignment surjection_rc = surjection;
    surjection_rc.set_sequence(reverse_complement(surjection.sequence()));
    
    // Align the old alignment to the graph in both orientations. Apparently
    // align only does a single oriantation, and we have no idea, even looking
    // at the mappings, which of the orientations will correspond to the one the
    // alignment is actually in.

    auto surjection_forward = align_to_graph(surjection, graph, max_query_graph_ratio);
    auto surjection_reverse = align_to_graph(surjection_rc, graph, max_query_graph_ratio);

#ifdef debug
#pragma omp critical (cerr)
    cerr << surjection.name() << " " << surjection_forward.score() << " forward score, " << surjection_reverse.score() << " reverse score" << endl;
#endif
    
    if(surjection_reverse.score() > surjection_forward.score()) {
        // Even if we have to surject backwards, we have to send the same string out as we got in.
        surjection = reverse_complement_alignment(surjection_reverse, node_length);
    } else {
        surjection = surjection_forward;
    }
    
    
#ifdef debug

#pragma omp critical (cerr)
        cerr << surjection.path().mapping_size() << " mappings, " << kept_paths.size() << " paths" << endl;

#endif

    if (surjection.path().mapping_size() > 0 && kept_paths.size() == 1) {
        // determine the paths of the node we mapped into
        //  ... get the id of the first node, get the paths of it
        assert(kept_paths.size() == 1);
        path_name = *kept_paths.begin();

        int64_t path_id = xindex->path_rank(path_name);
        auto& first_pos = surjection.path().mapping(0).position();
        int64_t hit_id = surjection.path().mapping(0).position().node_id();
        bool hit_backward = surjection.path().mapping(0).position().is_reverse();
        // we pick up positional information using the index

        auto path_posns = xindex->position_in_path(hit_id, path_name);
        if (path_posns.size() > 1) {
            cerr << "[vg map] surject_alignment: warning, multiple positions for node " << hit_id << " in " << path_name << " but will use only first: " << path_posns.front() << endl;
        } else if (path_posns.size() == 0) {
            cerr << "[vg map] surject_alignment: error, no positions for alignment " << source.name() << endl;
            exit(1);
        }

        // if we are reversed
        path_pos = path_posns.front();
        bool reversed_path = xindex->mapping_at_path_position(path_name, path_pos).position().is_reverse();
        if (reversed_path) {
            // if we got the start of the node position relative to the path
            // we need to offset to make thinsg right
            // but which direction
            if (hit_backward) {
                path_pos = path_posns.front() + first_pos.offset();
            } else {
                auto pos = reverse_complement_alignment(surjection, node_length).path().mapping(0).position();
                path_pos = xindex->position_in_path(pos.node_id(), path_name).front() + pos.offset();
            }
            path_reverse = !hit_backward;
        } else {
            if (!hit_backward) {
                path_pos = path_posns.front() + first_pos.offset();
            } else {
                auto pos = reverse_complement_alignment(surjection, node_length).path().mapping(0).position();
                path_pos = xindex->position_in_path(pos.node_id(), path_name).front() + pos.offset();
            }
            path_reverse = hit_backward;
        }

    } else {

        surjection = source;
#ifdef debug

#pragma omp critical (cerr)
        cerr << "Alignment " << source.name() << " did not align to the surjection subgraph" << endl;

#endif

    }

    return surjection;
}

const int balanced_stride(int read_length, int kmer_size, int stride) {
    double r = read_length;
    double k = kmer_size;
    double j = stride;
    int i = (r > j) ? round((r-k)/round((r-k)/j)) : j;
    return max(1, i);
}

const vector<string> balanced_kmers(const string& seq, const int kmer_size, const int stride) {
    // choose the closest stride that will generate balanced kmers
    vector<string> kmers;
    int b = balanced_stride(seq.size(), kmer_size, stride);
    if (!seq.empty()) {
        for (int i = 0; i+kmer_size <= seq.size(); i+=b) {
            kmers.push_back(seq.substr(i,kmer_size));
        }
    }
    return kmers;
}

bool operator==(const MaximalExactMatch& m1, const MaximalExactMatch& m2) {
    return m1.begin == m2.begin && m1.end == m2.end && m1.nodes == m2.nodes;
}

bool operator<(const MaximalExactMatch& m1, const MaximalExactMatch& m2) {
    return m1.begin < m2.begin && m1.end < m2.end && m1.nodes < m2.nodes;
}

MEMChainModel::MEMChainModel(
    const vector<size_t>& aln_lengths,
    const vector<vector<MaximalExactMatch> >& matches,
    Mapper* mapper,
    const function<double(const MaximalExactMatch&, const MaximalExactMatch&)>& transition_weight,
    int band_width,
    int position_depth,
    int max_connections) {
    // store the MEMs in the model
    int frag_n = 0;
    for (auto& fragment : matches) {
        ++frag_n;
        for (auto& mem : fragment) {
            // copy the MEM for each specific hit in the base graph
            // and add it in as a vertex
            for (auto& node : mem.nodes) {
                //model.emplace_back();
                //auto m = model.back();
                MEMChainModelVertex m;
                m.mem = mem;
                m.weight = mem.length();
                m.prev = nullptr;
                m.score = 0;
                m.approx_position = mapper->approx_position(make_pos_t(node));
                m.mem.nodes.clear();
                m.mem.nodes.push_back(node);
                m.mem.fragment = frag_n;
                m.mem.match_count = mem.match_count;
                model.push_back(m);
            }
        }
    }
    // index the model with the positions
    for (vector<MEMChainModelVertex>::iterator v = model.begin(); v != model.end(); ++v) {
        approx_positions[v->approx_position].push_back(v);
    }
    // sort the vertexes at each approx position by their matches and trim
    for (auto& pos : approx_positions) {
        std::sort(pos.second.begin(), pos.second.end(), [](const vector<MEMChainModelVertex>::iterator& v1,
                                                           const vector<MEMChainModelVertex>::iterator& v2) {
                      return v1->mem.match_count < v2->mem.match_count;
                  });
        pos.second.resize(min(pos.second.size(), (size_t)position_depth));
    }
    // for each vertex merge if we go equivalently forward in the positional space and forward in the read to the next position
    // scan forward
    set<vector<MEMChainModelVertex>::iterator> redundant_vertexes;
    for (map<int, vector<vector<MEMChainModelVertex>::iterator> >::iterator p = approx_positions.begin();
         p != approx_positions.end(); ++p) {
        for (auto& v1 : p->second) {
            if (redundant_vertexes.count(v1)) continue;
            auto q = p;
            while (++q != approx_positions.end() && abs(p->first - q->first) < band_width) {
                for (auto& v2 : q->second) {
                    if (redundant_vertexes.count(v2)) continue;
                    if (mems_overlap(v1->mem, v2->mem)
                        && abs(v2->mem.begin - v1->mem.begin) == abs(q->first - p->first)) {
                        v1->mem.end = v2->mem.end;
                        v1->weight = v1->mem.length();
                        redundant_vertexes.insert(v2);
                    }
                }
            }
        }
    }
    // scan reverse
    for (map<int, vector<vector<MEMChainModelVertex>::iterator> >::reverse_iterator p = approx_positions.rbegin();
         p != approx_positions.rend(); ++p) {
        for (auto& v1 : p->second) {
            if (redundant_vertexes.count(v1)) continue;
            auto q = p;
            while (++q != approx_positions.rend() && abs(p->first - q->first) < band_width) {
                for (auto& v2 : q->second) {
                    if (redundant_vertexes.count(v2)) continue;
                    if (mems_overlap(v1->mem, v2->mem)
                        && abs(v2->mem.begin - v1->mem.begin) == abs(p->first - q->first)) {
                        v1->mem.end = v2->mem.end;
                        v1->weight = v1->mem.length();
                        redundant_vertexes.insert(v2);
                    }
                }
            }
        }
    }
    // now build up the model using the positional bandwidth
    for (map<int, vector<vector<MEMChainModelVertex>::iterator> >::iterator p = approx_positions.begin();
         p != approx_positions.end(); ++p) {
        // look bandwidth before and bandwidth after in the approx positions
        // after
        for (auto& v1 : p->second) {
            if (redundant_vertexes.count(v1)) continue;
            auto q = p;
            while (++q != approx_positions.end() && abs(p->first - q->first) < band_width) {
                for (auto& v2 : q->second) {
                    if (redundant_vertexes.count(v2)) continue;
                    // if this is an allowable transition, run the weighting function on it
                    if (v1->next_cost.size() < max_connections
                        && v2->prev_cost.size() < max_connections) {
                        if (v1->mem.fragment < v2->mem.fragment
                            || v1->mem.begin < v2->mem.begin) {
                            double weight = transition_weight(v1->mem, v2->mem);
                            if (weight > -std::numeric_limits<double>::max()) {
                                v1->next_cost.push_back(make_pair(&*v2, weight));
                                v2->prev_cost.push_back(make_pair(&*v1, weight));
                            }
                        } else if (v1->mem.fragment > v2->mem.fragment
                                   || v1->mem.begin > v2->mem.begin) {
                            double weight = transition_weight(v2->mem, v1->mem);
                            if (weight > -std::numeric_limits<double>::max()) {
                                v2->next_cost.push_back(make_pair(&*v1, weight));
                                v1->prev_cost.push_back(make_pair(&*v2, weight));
                            }
                        }
                    }
                }
            }
        }
    }
}

void MEMChainModel::score(const set<MEMChainModelVertex*>& exclude) {
    // propagate the scores in the model
    for (auto& m : model) {
        // score is equal to the max inbound + mem.weight
        if (exclude.count(&m)) continue; // skip if vertex was whole cluster
        m.score = m.weight;
        for (auto& p : m.prev_cost) {
            if (p.first == nullptr) continue; // this transition is masked out
            double proposal = m.weight + p.second + p.first->score;
            if (proposal > m.score) {
                m.prev = p.first;
                m.score = proposal;
            }
        }
    }
}

MEMChainModelVertex* MEMChainModel::max_vertex(void) {
    MEMChainModelVertex* maxv = nullptr;
    for (auto& m : model) {
        if (maxv == nullptr || m.score > maxv->score) {
            maxv = &m;
        }
    }
    return maxv;
}

void MEMChainModel::clear_scores(void) {
    for (auto& m : model) {
        m.score = 0;
        m.prev = nullptr;
    }
}

vector<vector<MaximalExactMatch> > MEMChainModel::traceback(int alt_alns, bool paired, bool debug) {
    vector<vector<MaximalExactMatch> > traces;
    traces.reserve(alt_alns); // avoid reallocs so we can refer to pointers to the traces
    set<MEMChainModelVertex*> exclude;
    for (int i = 0; i < alt_alns; ++i) {
        // score the model, accounting for excluded traces
        clear_scores();
        score(exclude);
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "MEMChainModel::traceback " << i << endl;
                display(cerr);
            }
        }
#endif
        vector<MEMChainModelVertex*> vertex_trace;
        {
            // find the maximum score
            auto* vertex = max_vertex();
            // check if we've exhausted our MEMs
            if (vertex == nullptr || vertex->score == 0) break;
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "maximum score " << vertex->mem.sequence() << " " << vertex << ":" << vertex->score << endl;
            }
#endif
            // make trace
            while (vertex != nullptr) {
                vertex_trace.push_back(vertex);
                if (vertex->prev != nullptr) {
                    vertex = vertex->prev;
                } else {
                    break;
                }
            }
        }
        // if we have a singular match or reads are not paired, record not to use it again
        if (paired && vertex_trace.size() == 1) {
            exclude.insert(vertex_trace.front());
        }
        // fill this out when we're paired to help mask out in-fragment transitions
        set<MEMChainModelVertex*> chain_members;
        if (paired) for (auto v : vertex_trace) chain_members.insert(v);
        traces.emplace_back();
        auto& mem_trace = traces.back();
        for (auto v = vertex_trace.rbegin(); v != vertex_trace.rend(); ++v) {
            auto& vertex = **v;
            if (!paired) exclude.insert(&vertex);
            if (v != vertex_trace.rbegin()) {
                auto y = v - 1;
                MEMChainModelVertex* prev = *y;
                // mask out used transitions
                for (auto& p : vertex.prev_cost) {
                    if (p.first == prev) {
                        p.first = nullptr;
                    } else if (paired && p.first != nullptr
                               && p.first->mem.fragment != vertex.mem.fragment
                               && chain_members.count(p.first)) {
                        p.first = nullptr;
                    }
                }
            }
            mem_trace.push_back(vertex.mem);
        }
    }
    return traces;
}

// show model
void MEMChainModel::display(ostream& out) {
    for (auto& vertex : model) {
        out << vertex.mem.sequence() << ":" << vertex.mem.fragment << " " << &vertex << ":" << vertex.score << "@";
        for (auto& node : vertex.mem.nodes) {
            id_t id = gcsa::Node::id(node);
            size_t offset = gcsa::Node::offset(node);
            bool is_rev = gcsa::Node::rc(node);
            out << id << (is_rev ? "-" : "+") << ":" << offset << " ";
        }
        out << "prev: ";
        for (auto& p : vertex.prev_cost) {
            auto& next = p.first;
            if (p.first == nullptr) continue;
            out << p.first << ":" << p.second << "@";
            for (auto& node : next->mem.nodes) {
                id_t id = gcsa::Node::id(node);
                size_t offset = gcsa::Node::offset(node);
                bool is_rev = gcsa::Node::rc(node);
                out << id << (is_rev ? "-" : "+") << ":" << offset << " ";
            }
            out << " ; ";
        }
        out << " next: ";
        for (auto& p : vertex.next_cost) {
            auto& next = p.first;
            if (p.first == nullptr) continue;
            out << p.first << ":" << p.second << "@";
            for (auto& node : next->mem.nodes) {
                id_t id = gcsa::Node::id(node);
                size_t offset = gcsa::Node::offset(node);
                bool is_rev = gcsa::Node::rc(node);
                out << id << (is_rev ? "-" : "+") << ":" << offset << " ";
            }
            out << " ; ";
        }
        out << endl;
    }
}

// construct the sequence of the MEM; useful in debugging
string MaximalExactMatch::sequence(void) const {
    string seq; //seq.resize(end-begin);
    string::const_iterator c = begin;
    while (c != end) seq += *c++;
    return seq;
}
    
// length of the MEM
int MaximalExactMatch::length(void) const {
    return end - begin;
}

// uses an xgindex to fill out the MEM positions
void MaximalExactMatch::fill_positions(Mapper* mapper) {
    for (auto& node : nodes) {
        positions = mapper->node_positions_in_paths(gcsa::Node::encode(gcsa::Node::id(node), 0, gcsa::Node::rc(node)));
    }
}

// counts Ns in the MEM
size_t MaximalExactMatch::count_Ns(void) const {
    return std::count(begin, end, 'N');
}

ostream& operator<<(ostream& out, const MaximalExactMatch& mem) {
    size_t len = mem.begin - mem.end;
    out << mem.sequence() << ":";
    for (auto& node : mem.nodes) {
        id_t id = gcsa::Node::id(node);
        size_t offset = gcsa::Node::offset(node);
        bool is_rev = gcsa::Node::rc(node);
        out << id << (is_rev ? "-" : "+") << ":" << offset << ",";
    }
    return out;
}
    

}
