#ifndef VG_VARIANT_ADDER_HPP
#define VG_VARIANT_ADDER_HPP

/** \file
 * variant_adder.hpp: defines a tool class used for adding variants from VCF
 * files into existing graphs.
 */

#include "vcf_buffer.hpp"
#include "path_index.hpp"
#include "vg.hpp"

namespace vg {

using namespace std;

/**
 * A tool class for adding variants to a VG graph.
 */
class VariantAdder {

public:
    
    /**
     * Make a new VariantAdder to add variants to the given graph. Modifies the
     * graph in place.
     */
    VariantAdder(VG& graph);
    
    /**
     * Add in the variants from the given non-null VCF file. The file must be
     * freshly opened. The variants in the file must be sorted.
     *
     * Each file of variants is added as a batch.
     */
    void add_variants(vcflib::VariantCallFile* vcf);
    
    /// How wide of a range in bases should we look for nearby variants in?
    size_t variant_range = 100;
    
    /// How much additional context should we try and add outside the variants
    /// we grab? This sin't added to variant_range; it's counted from the outer
    /// ends of variants actually found.
    size_t flank_range = 100;
    
protected:
    /// The graph we are modifying
    VG& graph;
    
    /// We need indexes of all the paths that variants happen on. This holds a
    /// PathIndex for each path we touch by path name.
    map<string, PathIndex> indexes;
    
    /**
     * Get the index for the given path name.
     */
    PathIndex& get_path_index(const string& path_name);
    
    /**
     * Get all the unique combinations of variant alts represented by actual
     * haplotypes. Arbitrarily phases unphased variants.
     *
     * Returns a set of vectors or one number per variant, giving the alt number
     * (starting with 0 for reference) that appears on the haplotype.
     */
    set<vector<int>> get_unique_haplotypes(const vector<vcflib::Variant*>& variants) const;
    
    /**
     * Convert a haplotype on a list of variants into a string. The string will
     * run from the start of the first variant through the end of the last
     * variant.
     */
    string haplotype_to_string(const vector<int>& haplotype, const vector<vcflib::Variant*>& variants);
 

};

}

#endif

