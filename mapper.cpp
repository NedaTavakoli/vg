#include "mapper.hpp"

namespace vg {

Mapper::Mapper(Index* idex, gcsa::GCSA* g)
    : index(idex)
    , gcsa(g)
    , best_clusters(0)
    , cluster_min(2)
    , hit_max(100)
    , hit_size_threshold(0)
    , kmer_min(11)
    , kmer_threshold(1)
    , kmer_sensitivity_step(3)
    , thread_extension(1)
    , thread_extension_max(80)
    , max_thread_gap(30)
    , max_attempts(7)
    , softclip_threshold(0)
    , prefer_forward(false)
    , greedy_accept(false)
    , target_score_per_bp(1.5)
    , min_kmer_entropy(0)
    , debug(false)
{
    kmer_sizes = index->stored_kmer_sizes();
    if (kmer_sizes.empty() && gcsa == NULL) {
        cerr << "error:[vg::Mapper] the index (" 
             << index->name << ") does not include kmers"
             << " and no GCSA index has been provided" << endl;
        exit(1);
    }
}

Mapper::~Mapper(void) {
    // noop
}

Alignment Mapper::align(string& seq, int kmer_size, int stride, int band_width) {
    Alignment aln;
    aln.set_sequence(seq);
    return align(aln, kmer_size, stride, band_width);
}

// align read2 near read1's mapping location
void Mapper::align_mate_in_window(Alignment& read1, Alignment& read2, int pair_window) {
    if (read1.score() == 0) return; // bail out if we haven't aligned the first
    // try to recover in region
    Path* path = read1.mutable_path();
    int64_t idf = path->mutable_mapping(0)->position().node_id();
    int64_t idl = path->mutable_mapping(path->mapping_size()-1)->position().node_id();
    // but which way should we expand? this will make things much easier
    // just use the whole "window" for now
    int64_t first = max((int64_t)0, idf - pair_window);
    int64_t last = idl + (int64_t) pair_window;
    VG* graph = new VG;
    index->get_range(first, last, *graph);
    graph->remove_orphan_edges();
    read2.clear_path();
    read2.set_score(0);
    
    graph->align(read2);
    delete graph;
}

pair<Alignment, Alignment> Mapper::align_paired(Alignment& read1, Alignment& read2, int kmer_size, int stride, int band_width, int pair_window) {

    // use paired-end resolution techniques
    //
    // attempt mapping of first mate
    // if it works, expand the search space for the second (try to avoid new kmer lookups)
    //     (alternatively, do the whole kmer lookup thing but then restrict the constructed
    //      graph to a range near the first alignment)
    // if it doesn't work, try the second, then expand the range to align the first
    //
    // problem: need to develop model of pair orientations
    // solution: collect a buffer of alignments and then align them using unpaired approach
    //           detect read orientation and mean (and sd) of pair distance

    //if (try_both_first) {
    Alignment aln1 = align(read1, kmer_size, stride, band_width);
    Alignment aln2 = align(read2, kmer_size, stride, band_width);
    // link the fragments
    aln1.mutable_fragment_next()->set_name(aln2.name());
    aln2.mutable_fragment_prev()->set_name(aln1.name());
    // and then try to rescue unmapped mates
    if (aln1.score() == 0 && aln2.score()) {
        // should we reverse the read??
        if (aln2.is_reverse()) {
            aln1.set_sequence(reverse_complement(aln1.sequence()));
            aln1.set_is_reverse(true);
        }
        align_mate_in_window(aln2, aln1, pair_window);
    } else if (aln2.score() == 0 && aln1.score()) {
        if (aln1.is_reverse()) {
            aln2.set_sequence(reverse_complement(aln2.sequence()));
            aln2.set_is_reverse(true);
        }
        align_mate_in_window(aln1, aln2, pair_window);
    }
    // TODO
    // mark them as discordant if there is an issue?
    // this needs to be detected with care using statistics built up from a bunch of reads
    return make_pair(aln1, aln2);

}

Alignment Mapper::align_banded(Alignment& read, int kmer_size, int stride, int band_width) {
    // split the alignment up into overlapping chunks of band_width size
    list<Alignment> alignments;
    assert(read.sequence().size() > band_width);
    int div = 2;
    while (read.sequence().size()/div > band_width) {
        ++div;
    }
    int segment_size = read.sequence().size()/div;
    // and overlap them too
    Alignment merged;
    for (int i = 0; i < div; ++i) {
        {
            Alignment aln = read;
            if (i+1 == div) {
                // ensure we get all the sequence
                aln.set_sequence(read.sequence().substr(i*segment_size));
            } else {
                aln.set_sequence(read.sequence().substr(i*segment_size, segment_size));
            }
            if (i == 0) {
                merged = align(aln, kmer_size, stride);
            } else {
                merge_alignments(merged, align(aln, kmer_size, stride));
            }
        }
        // and the overlapped bit --- here we're using 50% overlap
        if (i != div-1) { // if we're not at the last sequence
            Alignment aln = read;
            aln.set_sequence(read.sequence().substr(i*segment_size+segment_size/2, segment_size));
            merge_alignments(merged, align(aln, kmer_size, stride));
        }
    }
    return merged;
}

Alignment Mapper::align(Alignment& aln, int kmer_size, int stride, int band_width) {

    if (aln.sequence().size() > band_width) {
        return align_banded(aln, kmer_size, stride, band_width);
    }

    std::chrono::time_point<std::chrono::system_clock> start_both, end_both;
    if (debug) start_both = std::chrono::system_clock::now();
    const string& sequence = aln.sequence();

    // if kmer size is not specified, pick it up from the index
    // for simplicity, use the first available kmer size; this could change
    if (kmer_size == 0) kmer_size = *kmer_sizes.begin();
    // and start with stride such that we barely cover the read with kmers
    if (stride == 0)
        stride = sequence.size()
            / ceil((double)sequence.size() / kmer_size);

    int kmer_hit_count = 0;
    int kept_kmer_count = 0;

    if (debug) cerr << "aligning " << aln.sequence() << endl;

    // forward
    Alignment alignment_f = aln;

    // reverse
    Alignment alignment_r = aln;
    alignment_r.set_sequence(reverse_complement(aln.sequence()));
    alignment_r.set_is_reverse(true);

    auto increase_sensitivity = [this,
                                 &kmer_size,
                                 &stride,
                                 &sequence,
                                 &alignment_f,
                                 &alignment_r]() {
        kmer_size -= kmer_sensitivity_step;
        stride = sequence.size() / ceil((double)sequence.size() / kmer_size);
        if (debug) cerr << "realigning with " << kmer_size << " " << stride << endl;
        /*
        if ((double)stride/kmer_size < 0.5 && kmer_size -5 >= kmer_min) {
            kmer_size -= 5;
            stride = sequence.size() / ceil((double)sequence.size() / kmer_size);
            if (debug) cerr << "realigning with " << kmer_size << " " << stride << endl;
        } else if ((double)stride/kmer_size >= 0.5 && kmer_size >= kmer_min) {
            stride = max(1, stride/3);
            if (debug) cerr << "realigning with " << kmer_size << " " << stride << endl;
        }
        */
    };

    int attempt = 0;
    int kmer_count_f = 0;
    int kmer_count_r = 0;

    while (alignment_f.score() == 0 && alignment_r.score() == 0 && attempt < max_attempts) {

        {
            std::chrono::time_point<std::chrono::system_clock> start, end;
            if (debug) start = std::chrono::system_clock::now();
            align_threaded(alignment_f, kmer_count_f, kmer_size, stride, attempt);
            if (debug) {
                end = std::chrono::system_clock::now();
                std::chrono::duration<double> elapsed_seconds = end-start;
                cerr << elapsed_seconds.count() << "\t" << "+" << "\t" << alignment_f.sequence() << endl;
            }
        }

        if (!(prefer_forward && (float)alignment_f.score() / (float)sequence.size() >= target_score_per_bp))
        {
            std::chrono::time_point<std::chrono::system_clock> start, end;
            if (debug) start = std::chrono::system_clock::now();
            align_threaded(alignment_r, kmer_count_r, kmer_size, stride, attempt);
            if (debug) {
                end = std::chrono::system_clock::now();
                std::chrono::duration<double> elapsed_seconds = end-start;
                cerr << elapsed_seconds.count() << "\t" << "-" << "\t" << alignment_r.sequence() << endl;
            }
        }

        ++attempt;

        if (alignment_f.score() == 0 && alignment_r.score() == 0) {
            increase_sensitivity();
        } else {
            break;
        }

    }

    if (debug) {
        end_both = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed_seconds = end_both-start_both;
        cerr << elapsed_seconds.count() << "\t" << "b" << "\t" << sequence << endl;
    }

    if (alignment_r.score() > alignment_f.score()) {
        return alignment_r;
    } else {
        return alignment_f;
    }
}

Alignment& Mapper::align_threaded(Alignment& alignment, int& kmer_count, int kmer_size, int stride, int attempt) {

    // parameters, some of which should probably be modifiable
    // TODO -- move to Mapper object

    if (index == NULL) {
        cerr << "error:[vg::Mapper] no index loaded, cannot map alignment!" << endl;
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
        //if (debug) cerr << "kmer " << k << " entropy = " << entropy(k) << endl;
        if (min_kmer_entropy > 0 && entropy(k) < min_kmer_entropy) continue;
        uint64_t approx_matches = index->approx_size_of_kmer_matches(k);
        // Report the approximate match count
        if (debug) cerr << k << "\t~" << approx_matches << endl;
        // if we have more than one block worth of kmers on disk, consider this kmer non-informative
        // we can do multiple mapping by relaxing this
        if (approx_matches > hit_size_threshold) {
            continue;
        }
        
        // Grab the map from node ID to kmer start positions for this particular kmer.
        auto& kmer_positions = positions.at(i);
        // Fill it in, since we know there won't be too many to work with.
        index->get_kmer_positions(k, kmer_positions);
        // ignore this kmer if it has too many hits
        // typically this will be filtered out by the approximate matches filter
        if (kmer_positions.size() > hit_max) kmer_positions.clear();
        // Report the actual match count for the kmer
        if (debug) cerr << "\t=" << kmer_positions.size() << endl;
        kmer_count += kmer_positions.size();
        // break when we get more than a threshold number of kmers to seed further alignment
        //if (kmer_count >= kmer_threshold) break;
        ++i;
    }

    if (debug) cerr << "kept kmer hits " << kmer_count << endl;

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

    int thread_ex = thread_extension;
    map<vector<int64_t>*, Alignment> alignments;

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
            // thread extension should be determined during iteration
            // note that there is a problem and hits tend to be imbalanced
            int64_t first = max((int64_t)0, *thread.begin() - thread_ex);
            int64_t last = *thread.rbegin() + thread_ex;
            //int64_t first = *thread.begin();
            //int64_t last = *thread.rbegin();
            // so we can pick it up efficiently from the index by pulling the range from first to last
            if (debug) cerr << "getting node range " << first << "-" << last << endl;
            VG* graph = new VG;
            index->get_range(first, last, *graph);
            Alignment& ta = alignments[&thread];
            ta = alignment;
            // by default, expand the graph a bit so we are likely to map
            //index->get_connected_nodes(*graph);
            graph->remove_orphan_edges();
            
            if (debug) cerr << "got subgraph with " << graph->node_count() << " nodes, " 
                            << graph->edge_count() << " edges" << endl;
                            
            // Topologically sort the graph, breaking cycles and orienting all edges end to start.
            // This flips some nodes around, so we need to translate alignments back.
            set<int64_t> flipped_nodes;
            graph->orient_nodes_forward(flipped_nodes);
                            
            // align
            ta.clear_path();
            ta.set_score(0);
            graph->align(ta);

            // check if we start or end with soft clips
            // if so, try to expand the graph until we don't have any more (or we hit a threshold)
            // expand in the direction where there were soft clips

            if (!ta.has_path()) continue;
            
            int sc_start = softclip_start(ta);
            int sc_end = softclip_end(ta);

            if (sc_start > softclip_threshold || sc_end > softclip_threshold) {
                if (debug) cerr << "softclip handling " << sc_start << " " << sc_end << endl;
                Path* path = ta.mutable_path();
                int64_t idf = path->mutable_mapping(0)->position().node_id();
                int64_t idl = path->mutable_mapping(path->mapping_size()-1)->position().node_id();
                // step towards the side where there were soft clips
                // using 10x the thread_extension
                int64_t f = max((int64_t)0, idf - (int64_t) max(thread_ex, 1) * 10);
                int64_t l = idl + (int64_t) max(thread_ex, 1) * 10;
                if (debug) cerr << "getting node range " << f << "-" << l << endl;

                { // always rebuild the graph
                    delete graph;
                    graph = new VG;
                }
                index->get_range(f, l, *graph);
                graph->remove_orphan_edges();
                
                if (debug) cerr << "got subgraph with " << graph->node_count() << " nodes, " 
                                << graph->edge_count() << " edges" << endl;
                                
                ta.clear_path();
                ta.set_score(0);
                graph->align(ta);
                if (debug) cerr << "softclip after " << softclip_start(ta) << " " << softclip_end(ta) << endl;
            }

            delete graph;
            
            if (debug) cerr << "score per bp is " << (float)ta.score() / (float)ta.sequence().size() << endl;
            if (greedy_accept && (float)ta.score() / (float)ta.sequence().size() >= target_score_per_bp) {
                if (debug) cerr << "greedy accept" << endl;
                accepted = true;
                break;
            }
        }
    }

    // now find the best alignment
    int sum_score = 0;
    double mean_score = 0;
    map<int, set<Alignment*> > alignment_by_score;
    for (auto& ta : alignments) {
        Alignment* aln = &ta.second;
        alignment_by_score[aln->score()].insert(aln);
    }

    // get the best alignment
    set<Alignment*>& best = alignment_by_score.rbegin()->second;
    //cerr << alignment_by_score.size() << endl;
    if (!alignment_by_score.empty()) {
        alignment = **best.begin();
        if (debug) {
            cerr << "best alignment score " << alignment.score() << endl;
        }
    } else {
        alignment.clear_path();
        alignment.set_score(0);
    }

    if (debug && alignment.score() == 0) cerr << "failed alignment" << endl;

    return alignment;

}

int softclip_start(Alignment& alignment) {
    if (alignment.mutable_path()->mapping_size() > 0) {
        Path* path = alignment.mutable_path();
        Mapping* first_mapping = path->mutable_mapping(0);
        Edit* first_edit = first_mapping->mutable_edit(0);
        if (first_edit->from_length() == 0 && first_edit->to_length() > 0) {
            return first_edit->to_length();
        }
    }
    return 0;
}

int softclip_end(Alignment& alignment) {
    if (alignment.mutable_path()->mapping_size() > 0) {
        Path* path = alignment.mutable_path();
        Mapping* last_mapping = path->mutable_mapping(path->mapping_size()-1);
        Edit* last_edit = last_mapping->mutable_edit(last_mapping->edit_size()-1);
        if (last_edit->from_length() == 0 && last_edit->to_length() > 0) {
            return last_edit->to_length();
        }
    }
    return 0;
}

Alignment& Mapper::align_simple(Alignment& alignment, int kmer_size, int stride) {

    if (index == NULL) {
        cerr << "error:[vg::Mapper] no index loaded, cannot map alignment!" << endl;
        exit(1);
    }

    // establish kmers
    const string& sequence = alignment.sequence();
    //  
    auto kmers = balanced_kmers(sequence, kmer_size, stride);

    map<string, int32_t> kmer_counts;
    vector<map<int64_t, vector<int32_t> > > positions(kmers.size());
    int i = 0;
    for (auto& k : kmers) {
        index->get_kmer_positions(k, positions.at(i++));
        kmer_counts[k] = positions.at(i-1).size();
    }
    positions.clear();
    VG* graph = new VG;
    for (auto& c : kmer_counts) {
        if (c.second < hit_max) {
            index->get_kmer_subgraph(c.first, *graph);
        }
    }

    int max_iter = sequence.size();
    int iter = 0;
    int context_step = 1;
    int64_t max_subgraph_size = 0;

    // use kmers which are informative
    // and build up the graph

    auto get_max_subgraph_size = [this, &max_subgraph_size, &graph]() {
        list<VG> subgraphs;
        graph->disjoint_subgraphs(subgraphs);
        for (auto& subgraph : subgraphs) {
            max_subgraph_size = max(subgraph.total_length_of_nodes(), max_subgraph_size);
        }
    };

    get_max_subgraph_size();

    while (max_subgraph_size < sequence.size()*2 && iter < max_iter) {
        index->expand_context(*graph, context_step);
        index->get_connected_nodes(*graph);
        get_max_subgraph_size();
        ++iter;
    }
    // ensure we have a complete graph prior to alignment
    index->get_connected_nodes(*graph);

    /*
    ofstream f("vg_align.vg");
    graph->serialize_to_ostream(f);
    f.close();
    */

    // Make sure the graph we're aligning to is all oriented
    graph->align(alignment);
    
    delete graph;

    return alignment;

}

const int balanced_stride(int read_length, int kmer_size, int stride) {
    double r = read_length;
    double k = kmer_size;
    double j = stride;
    if (r > j) {
        return round((r-k)/round((r-k)/j));
    } else {
        return j;
    }
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

}
