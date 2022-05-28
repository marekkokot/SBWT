#pragma once

#include "Kmer.hh"
#include <sdsl/bit_vectors.hpp>
#include "KMC/kmc_api/kmc_file.h"
#include "KMC/include/kmc_runner.h"
#include "KMC_code.hh"
#include "buffered_streams.hh"
#include "EM_sort/EM_sort.hh"
#include <set>
#include <unordered_map>
#include <stdexcept>

namespace sbwt{

namespace KMC_construction_helper_classes{

typedef long long LL;
typedef Kmer<MAX_KMER_LENGTH> kmer_t;

struct Node{
    kmer_t kmer;
    char edge_flags;

    Node() : kmer(), edge_flags(0) {}
    Node(kmer_t kmer) : kmer(kmer), edge_flags(0) {}

    static inline int64_t size_in_bytes(){
        return kmer_t::size_in_bytes() + sizeof(char); // char is the edge flags
    }

    void set(char c){
        if(c == 'A') edge_flags |= 1 << 0;
        else if(c == 'C') edge_flags |= 1 << 1;
        else if(c == 'G') edge_flags |= 1 << 2;
        else if(c == 'T') edge_flags |= 1 << 3;
    }

    bool has(char c) const{
        if(c == 'A') return edge_flags & (1 << 0);
        else if(c == 'C') return edge_flags & (1 << 1);
        else if(c == 'G') return edge_flags & (1 << 2);
        else if(c == 'T') return edge_flags & (1 << 3);
        return false;
    }

    bool operator==(const Node &other) const{
        return this->kmer == other.kmer && this->edge_flags == other.edge_flags;
    }

    bool operator!=(const Node &other) const{
        return !(*this == other);
    }

    bool operator<(const Node &other) const{
        if(this->kmer < other.kmer) return true;
        if(this->kmer == other.kmer && this->edge_flags < other.edge_flags) return true;
        return false;
    }

    string to_string() const{
        string S = kmer.to_string() + ": ";
        S += has('A') ? '1' : '0';
        S += has('C') ? '1' : '0';
        S += has('G') ? '1' : '0';
        S += has('T') ? '1' : '0';
        return S;
    }

    void serialize(char* buf){
        kmer.serialize(buf);
        buf[size_in_bytes()-1] = edge_flags;
    }

    void load(const char* buf){
        kmer.load(buf);
        edge_flags = buf[size_in_bytes()-1];
    }

};

class Argv{ // Class for turning a vector<string> into char**
private:

    // Forbid copying the class because it wont work right
    Argv(Argv const& other);
    Argv& operator=(Argv const& other);

public:

    char** array = NULL;
    int64_t size = 0;

    Argv(vector<string> v){
        array = (char**)malloc(sizeof(char*) * v.size());
        // Copy contents of v into array
        for(int64_t i = 0; i < v.size(); i++){
            char* s = (char*)malloc(sizeof(char) * (v[i].size() + 1)); // +1: space for '\0' at the end
            for(int64_t j = 0; j < v[i].size(); j++){
                s[j] = v[i][j]; // Can't use strcpy because s.c_str() is const
            }
            s[v[i].size()] = '\0';
            array[i] = s;
        }
        size = v.size();
    }

    ~Argv(){
        for(int64_t i = 0; i < size; i++) free(array[i]);
        free(array);
    }

};

// Also gives reverse compelments
class Kmer_stream_from_KMC_DB{

private:

    CKMCFile kmer_database;
    CKmerAPI kmer_object;

    uint32 _kmer_length;
    uint32 _mode;
    uint32 _counter_size;
    uint32 _lut_prefix_length;
    uint32 _signature_len;
    uint32 _min_count;
    uint64 _max_count;
    uint64 _total_kmers;

    bool add_revcomps;
    std::string str;
    std::string str_revcomp;
    bool revcomp_next = false;

    char get_rc(char c){
        switch(c){
            case 'A': return 'T';
            case 'T': return 'A';
            case 'C': return 'G';
            case 'G': return 'C';
            default: cerr << "Error getting reverse complement from " << c << endl; exit(1);
        }   
    }

    void reverse_complement(string& S){
        std::reverse(S.begin(), S.end());
        for(char& c : S) c = get_rc(c);
    }   

public:

    Kmer_stream_from_KMC_DB(string KMC_db_path, bool add_revcomps) : add_revcomps(add_revcomps) {
        if (!kmer_database.OpenForListing(KMC_db_path)){
            throw std::runtime_error("Error opening KMC database " + KMC_db_path);
        }

		kmer_database.Info(_kmer_length, _mode, _counter_size, _lut_prefix_length, _signature_len, _min_count, _max_count, _total_kmers);

        kmer_object = CKmerAPI(_kmer_length);
	
    }

    bool done(){
        return (!add_revcomps || !revcomp_next) && kmer_database.Eof();
    }
    
    Kmer<MAX_KMER_LENGTH> next(){
        if(add_revcomps && revcomp_next){
            revcomp_next = false;
            return Kmer<MAX_KMER_LENGTH>(str_revcomp);
        }

        //float counter_f;
        uint32 counter_i;
		/*if(_mode){ //quake compatible mode
			kmer_database.ReadNextKmer(kmer_object, counter_f);
		}
		else {			*/
			kmer_database.ReadNextKmer(kmer_object, counter_i);
		//}

        kmer_object.to_string(str);
        if(add_revcomps){
            str_revcomp = str;
            reverse_complement(str_revcomp);
            if(str != str_revcomp) revcomp_next = true;
        }

        std::reverse(str.begin(), str.end()); // Return reverses so that they are in colex order

        return Kmer<MAX_KMER_LENGTH>(str);

    }

};

class Disk_Instream{

private:

    // Have pointer members -> no copying
    Disk_Instream(Disk_Instream const&) = delete;
    Disk_Instream& operator=(Disk_Instream const&) = delete;

    bool all_read = false;
    Buffered_ifstream<> in;
    char* in_buffer;

    Node top;

    void update_top(){
        in.read(in_buffer, Node::size_in_bytes());
        if(in.eof()){
            all_read = true;
            return;
        }
        top.load(in_buffer);
    }

public:

    Disk_Instream(string filename) {
        in.open(filename, ios_base::binary);
        in_buffer = (char*)malloc(Node::size_in_bytes());
    }

    bool stream_done() const{
        return all_read;
    }

    Node stream_next(){
        Node ret = top;
        update_top();
        return ret;
    }

    Node peek_next(){
        return top;
    }

    ~Disk_Instream(){
        free(in_buffer);
    }

};

// A single sorted stream out of two sorted streams
class Node_stream_merger{

    Disk_Instream& A;
    Disk_Instream& B;

public:

    Node_stream_merger(Disk_Instream& A, Disk_Instream& B) : A(A), B(B){}

    bool stream_done(){
        return A.stream_done() && B.stream_done();
    }

    Node stream_next(){
        if(A.stream_done()) return B.stream_next();
        if(B.stream_done()) return A.stream_next();
        if(A.peek_next() < B.peek_next()) return A.stream_next();
        else return B.stream_next();
    }

};

} // End of namespace KMC_construction_helper_classes
} // End of namepace sbwt