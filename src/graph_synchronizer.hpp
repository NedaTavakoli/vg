#ifndef VG_GRAPH_SYNCHRONIZER_H
#define VG_GRAPH_SYNCHRONIZER_H

/**
 * \file graph_synchronizer.hpp: define a GraphSynchronizer that can manage
 * concurrent access and updates to a VG graph.
 */

#include "vg.hpp"
#include "path_index.hpp"
#include "shared_mutex.hpp"

#include <thread>
#include <mutex>
#include <condition_variable>

namespace vg {

using namespace std;

/**
 * Let threads get exclusive locks on subgraphs of a vg graph, for reading and
 * editing. Whan a subgraph is locked, a copy is accessible through the lock
 * object and the underlying graph can be edited (through the lock) without
 * affecting any other locked subgraphs.
 *
 * A thread may only hold a lock on a single subgraph at a time. Trying to lock
 * another subgraph while you already have a subgraph locked is likely to result
 * in a deadlock.
 */ 
class GraphSynchronizer {

public:

    /**
     * Create a GraphSynchronizer for synchronizing on parts of the given graph.
     */
    GraphSynchronizer(VG& graph);
    
    /**
     * Since internally we keep PathIndexes for paths in the graph, we expose
     * this method for getting the strings for paths.
     */
    const string& get_path_sequence(const string& path_name);
    
    /**
     * We can actually let users run whatever function they want with an
     * exclusive handle on a PathIndex, with the guarantee that the graph won't
     * change while they're working.
     */
    template <typename T>
    T with_path_index(const string& path_name, const function<T(const PathIndex&)>& to_run) {
        // Get a reader lock on the graph
        ting::shared_lock<ting::shared_mutex> guard(whole_graph_mutex);
        return to_run(get_path_index(path_name));
    }
    
    
    /**
     * This represents a request to lock a particular context on a particular
     * GraphSynchronizer. It fulfils the BasicLockable concept requirements, so
     * you can wait on it with std::unique_lock.
     */
    class Lock {
    public:
        
        /**
         * Create a request to lock a certain radius around a certain position
         * along a certain path in the graph controlled by the given
         * synchronizer.
         */
        Lock(GraphSynchronizer& synchronizer, const string& path_name, size_t path_offset, size_t context_bases, bool reflect);
        
        /**
         * Block until a lock is obtained.
         */
        void lock();
        // Locking is going to need a lock on locked_nodes_mutex and a reader
        // lock on the graph. This will block simultaneous lock() and unlock()
        // calls and simultaneous apply_edit() calls.
        
        /**
         * If a lock is held, unlock it.
         */
        void unlock();
        // Unlocking is going to need a lock on locked_nodes_mutex. This will
        // block simultaneous lock() and unlock() calls.
        
        /**
         * May only be called when locked. Grab the subgraph that was extracted
         * when the lock was obtained. Does not contain any path information.
         */
        VG& get_subgraph();
        
        /**
         * May only be called when locked. Apply an edit against the base graph
         * and return the resulting translation. Note that this updates only the
         * underlying VG graph, not the copy of the locked subgraph stored in
         * the lock. Also note that the edit may only edit locked nodes.
         *
         * Edit operations will create new nodes, and cannot delete nodes or
         * apply changes (other than dividing and connecting) to existing nodes.
         *
         * Any new nodes created are created already locked.
         */
        vector<Translation> apply_edit(const Path& path);
        // Applying an edit needs a writer lock on the graph. This will block
        // simultaneous lock() calls and simultaneous apply_edit() calls
        
    protected:
    
        /// This points back to the synchronizer we synchronize with when we get locked.
        GraphSynchronizer& synchronizer;
        
        // These hold the actual lock request we represent
        string path_name;
        size_t path_offset;
        size_t context_bases;
        bool reflect; // Should we bounce off node ends?
        
        /// This is the subgraph that got extracted during the locking procedure.
        VG subgraph;
        
        /// These are the nodes connected to the subgraph but not actually
        /// available for editing. We just need no one else to edit them.
        set<id_t> periphery;
        
        /// This is the set of nodes that this lock has currently locked.
        set<id_t> locked_nodes;
    };
    
protected:
    
    /// The graph we manage
    VG& graph;
    
    /// We use this to lock the whole graph.
    /// When we're scanning the graph to build a PathIndex, we hold a read lock.
    /// When we're modifying the graph, we hold a write lock.
    /// When we're searching for nodes to lock, we hold a read lock.
    ///
    /// Only ever used by this class and internal classes (monitor-style), so we
    /// don't need it to be a recursive mutex.
    ting::shared_mutex whole_graph_mutex;
    
    /// We use this to protect the set of locked nodes, and the wait_for_region
    /// condition variable. To lock or unlock nodes you need to be holding this
    /// mutex. To lock nodes you also need to have a read lock on the
    /// whole_graph_mutex, to ensure nobody is writing to the graph.
    mutex locked_nodes_mutex;
    
    /// This holds all the node IDs that are currently locked by someone
    set<id_t> locked_nodes;
    
    /// We have one condition variable where we have blocked all the threads
    /// that are waiting to lock subgraphs but couldn't the first time because
    /// we ran into already locked nodes. When nodes get unlocked, we wake them
    /// all up, and they each grab the mutex and check, one at a time, to see if
    /// they can have all their nodes this time.
    condition_variable wait_for_region;
    
    /// This reader-writer lock protects the map below. You need a reader lock
    /// to look up or use a PathIndex, and a writer lock to add a new PathIndex.
    /// Note that you *also* need to hold a reader lock on the graph while doing
    /// anything with path indexes, because the writer lock on the graph
    /// controls updates to existing indexes.
    ting::shared_mutex indexes_mutex;
    
    /// We need indexes of all the paths that someone might want to use as a
    /// basis for locking. This holds a PathIndex for each path we touch by path
    /// name.
    map<string, PathIndex> indexes;
    
    /**
     * Get the index for the given path name. Caller must hold a reader lock on
     * the graph, and should not hold any lock on the idnexes.
     */
    PathIndex& get_path_index(const string& path_name);
    
    /**
     * Update all the path indexes according to the given translations. Caller
     * must already hold a writer lock on the graph.
     */
    void update_path_indexes(const vector<Translation>& translations);
    
    


};


}

#endif
