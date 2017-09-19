#include "subcommand.hpp"
#include "../vg.hpp"
#include "../utility.hpp"
#include "../mapper.hpp"
#include "../stream.hpp"
#include "../alignment.hpp"

using namespace vg;
using namespace vg::subcommand;

void help_annotate(char** argv) {
    cerr << "usage: " << argv[0] << " annotate [options] >output.{gam,vg}" << endl
         << "    -x, --xg-name FILE     an xg index describing a graph" << endl
         << "    -b, --bed-name FILE    a bed file describing a subpath" << endl
         << "    -d, --db-name DIR      a rocksdb index of a GAM" << endl
         << "    -v, --vg FILE          annotate this graph" << endl
         << "    -g, --gcsa FILE        a GCSA2 index file base name" << endl
         << "    -a, --gam FILE         alignments to annotate" << endl
         << "    -p, --positions        annotate alignments with reference positions" << endl
         << "    -i, --init-pos         use initial position of alignment instead of mean" << endl;
}

int main_annotate(int argc, char** argv) {
    
    if (argc == 2) {
        help_annotate(argv);
        return 1;
    }

    string xg_name;
    string rocksdb_name;
    string gcsa_name;
    string vg_name;
    string bed_name;
    string gam_name;
    bool add_positions = false;
    bool init_pos = false;

    int c;
    optind = 2; // force optind past command positional argument
    while (true) {
        static struct option long_options[] =
        {
            {"gam", required_argument, 0, 'a'},
            {"gcsa", required_argument, 0, 'g'},
            {"positions", no_argument, 0, 'p'},
            {"vg", required_argument, 0, 'v'},
            {"xg-name", required_argument, 0, 'x'},
            {"bed-name", required_argument, 0, 'b'},
            {"db-name", required_argument, 0, 'd'},
            {"init-pos", no_argument, 0, 'i'},
            {0, 0, 0, 0}
        };

        int option_index = 0;
        c = getopt_long (argc, argv, "hx:d:v:g:a:pib:",
                long_options, &option_index);

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c)
        {
        case 'd':
            rocksdb_name = optarg;
            break;

        case 'x':
            xg_name = optarg;
            break;

        case 'v':
            vg_name = optarg;
            break;

        case 'g':
            gcsa_name = optarg;
            break;

        case 'a':
            gam_name = optarg;
            break;

        case 'b':
            bed_name = optarg;
            break;

        case 'p':
            add_positions = true;
            break;
            
        case 'i':
            init_pos = true;
            break;

        case 'h':
        case '?':
            help_annotate(argv);
            exit(1);
            break;

        default:
            abort ();
        }
    }
    xg::XG* xg_index = nullptr;
    if (!xg_name.empty()) {
        ifstream in(xg_name);
        xg_index = new xg::XG(in);
    } else {
        cerr << "error [vg annotate]: no xg index provided" << endl;
        return 1;
    }
    
    Mapper mapper(xg_index, nullptr, nullptr);
    
    if (!gam_name.empty()) {
        vector<Alignment> buffer;
        if (add_positions) {
            //map<string, double> Mapper::alignment_mean_path_positions(const Alignment& aln, bool first_hit_only);
            function<void(Alignment&)> lambda = [&](Alignment& aln) {
                if (init_pos) {
                    mapper.annotate_with_initial_path_positions(aln);
                }
                else {
                    for (auto& ref : mapper.alignment_mean_path_positions(aln)) {
                        Position* refpos = aln.add_refpos();
                        refpos->set_name(ref.first);
                        refpos->set_offset(round(ref.second));
                    }
                }
                buffer.push_back(aln);
                stream::write_buffered(cout, buffer, 100);
            };
            get_input_file(gam_name, [&](istream& in) {
                    stream::for_each(in, lambda);
                });
            stream::write_buffered(cout, buffer, 0); // flush
        }
    } else if (!bed_name.empty()) {
        vector<Alignment> buffer;
        if (add_positions) {
            ifstream bed_stream(bed_name.c_str());

            parse_bed_regions(bed_stream, xg_index, &buffer);
            stream::write_buffered(cout, buffer, 0); // flush
        }
    } else {
        cerr << "only GAM or BED annotation is implemented" << endl;
        return 1;
    }

    if (xg_index) {
        delete xg_index;
    }
    
    return 0;
}

static Subcommand vg_annotate("annotate", "annotate alignments with graphs and graphs with alignments",
                              main_annotate);
