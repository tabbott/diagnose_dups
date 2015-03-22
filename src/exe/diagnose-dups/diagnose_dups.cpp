/*
may need to code against htslib rather than samtools.

For each read in the BAM file:
    calculate the 5' ends of the fragment:
    if last 5' end seen is less than the current 5' end:
        flush buffers, gathering stats on duplicates
    if last 5' end seen is greater than the current 5' end BAM is unsorted and freak out
    create signature and add to buffer

on buffer flush:
    for each signatures:
        skip if count is 1
        add number of reads in signature to histogram of duplicate fragments
        sort into bins by tile
        for each tile, create distance matrix
            increment counts in distance histogram
    clear out signatures hash

for each read we need:
    read name
    chromosome
    position
    a populated mate cigar tag (MC:Z:)
    tlen

we need a method to parse out readnames
we need a method to determine how far apart two readnames are
we need a method to generate a signature
we need a method to expand the cigar strings for the signature
we need a histogram template

we need a small amount of test data

diagnose_dups -i bam_file -o dup_stats 
*/

#include "common/Options.hpp"
#include "diagnose_dups/Read.hpp"
#include "diagnose_dups/Signature.hpp"

#include <boost/unordered_map.hpp>

#include <sam.h>
#include <iostream>

using namespace std;

int main(int argc, char** argv) {
    Options opts = Options(argc, argv);

    htsFile *fp = hts_open(opts.vm["input"].as<string>().c_str(), "r");
    if(fp == 0) {
       cerr << "Unable to open " << opts.vm["input"].as<string>() << "\n";
       exit(1);
    }
    bam_hdr_t *header = sam_hdr_read(fp);
    bam1_t *record = bam_init1();
    
    typedef vector<Read> read_vector;
    typedef boost::unordered_map<Signature, read_vector> signature_map;
    signature_map signatures;

    int32_t last_pos = -1;
    int32_t last_tid = -1;
    int32_t pos_window = 300;

    typedef boost::unordered_map<int, int> histogram;
    histogram dup_insert_sizes;
    histogram nondup_insert_sizes;
    histogram distances;
    histogram number_of_dups;

    Read read;
    while(sam_read1(fp, header, record) >= 0) {
        //skip non-properly paired alignments, they're not going to tell us anything about dups
        //as well as anything secondary, qcfail or supplementary
        if(!(record->core.flag & BAM_FPROPER_PAIR)
                || (record->core.flag & (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FSUPPLEMENTARY) ) 
                || (record->core.flag & BAM_FREAD2)) {
            continue;
        }

        if (!parse_read(record, read)) {
            std::cerr << "Failed to parse bam record, name = " << bam_get_qname(record) << "\n";
            // XXX: would you rather abort?
            continue;
        }

        Signature sig(record);
        if(last_pos > -1) {
            //grab iterators
            //iterate over sigs in signatures
            for(signature_map::iterator i = signatures.begin(); i != signatures.end(); ++i) {

                if(last_tid != sig.tid 
                        || (last_tid == i->first.tid && (last_pos - pos_window) > i->first.pos)) {
                    if(i->second.size() > 1) {
                        //it's a dup

                        //add up number of dups
                        number_of_dups[i->second.size()] += 1;

                        typedef vector<Read>::iterator read_vec_iter;
                        for(read_vec_iter current_read_iter = i->second.begin(); current_read_iter != i->second.end() - 1; ++current_read_iter) {
                            dup_insert_sizes[abs(current_read_iter->insert_size)] += 1;
                            nondup_insert_sizes[abs(current_read_iter->insert_size)] += 0;
                            for(read_vec_iter distance_calc_iter = current_read_iter + 1; distance_calc_iter != i->second.end(); ++distance_calc_iter) {
                                if(is_on_same_tile(*current_read_iter, *distance_calc_iter)) {
                                    uint64_t flow_cell_distance = euclidean_distance(*current_read_iter, *distance_calc_iter);
                                    distances[flow_cell_distance] += 1;
                                }
                            }
                        }
                    }
                    else {
                        nondup_insert_sizes[abs(i->second[0].insert_size)] += 1;
                        dup_insert_sizes[abs(i->second[0].insert_size)] += 0;
                    }
                    signatures.erase(i);
                }
            }
        }
        signatures[sig].push_back(read);
        last_tid = sig.tid;
        last_pos = sig.pos;
    }
    bam_destroy1(record);
    bam_hdr_destroy(header);
    hts_close(fp);

    cout << "Inter-tile distance\tFrequency\n";
    for(histogram::iterator i = distances.begin(); i != distances.end(); ++i) {
        cout << i->first << "\t" << i->second << "\n";
    }
    cout << "\n";

    cout << "Number of dups at location\tFrequency\n";
    for(histogram::iterator i = number_of_dups.begin(); i != number_of_dups.end(); ++i) {
        cout << i->first << "\t" << i->second << "\n";
    }
    cout << "\n";

    cout << "Size\tUniq frequency\tDup Frequency\n";
    for(histogram::iterator i = nondup_insert_sizes.begin(); i != nondup_insert_sizes.end(); ++i) {
        cout << i->first << "\t" << i->second << "\t" << dup_insert_sizes[i->first] << "\n";
    }

    return 0;
}
