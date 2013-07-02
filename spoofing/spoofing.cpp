/**
 * \file spoofing.cpp
 * \brief IP spoofing detector for Nemea
 * \author Roman Vrana, xvrana20@stud.fit.vutbr.cz
 * \date 2013
 */

#include <string>
#include <cctype>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <stdint.h>
#include <signal.h>
#include <getopt.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <libtrap/trap.h>
#ifdef __cplusplus
}
#endif
#include "../unirec.h"
#include "../ipaddr.h"
#include "spoofing.h"

#define DEBUG 1


using namespace std;

trap_module_info_t module_info = {
    "IP spoofing detection module", // Module name
    // Module description
    "This module checks ip addresses in data flows for possible IP spoofing.\n"
    "It uses four conditions to determine this:\n"
    "1. Testing for bogon prefixes\n"
    "2. Checking symetric routes\n"
    "3. Checking asymetric routes\n"
    "4. Counting new flows\n"
    "Interfaces:\n"
    "   Inputs: 1 (ur_basic_flow)\n"
    "   Outputs: 1\n" // will be 1 (ur_basic_flow_t)
    "Additional parameters:\n"
    "   -b <filename>    File with list of bogon prefixes. This parameter is \n"
    "                    mandatory.\n"
    "   -c <filename>    File with other specific prefixes.\n"
    "   -s <sec>         Time before updating records for symetric routing \n"
    "                    filter. Default value is 45 seconds.\n"
    "   -t <num>         Threshold for reporting spoofed addresses from new \n"
    "                    filter. Default value is 1000 flows.\n",
    1, // Number of input interfaces
    1, // Number of output interfaces (will be 1)
};

// *****    Bloom filter handling    *****

static int stop = 0;
static int bf_active = 0;
static int bf_learning = 1;

void inline swap_filters() 
{
    int tmp;
    tmp = bf_learning;
    bf_learning = bf_active;
    bf_active = tmp;
}

void create_nflow_filters(int length, flow_filter_t* filters)
{
    for (int i = 0; i < length; i++) {
        bloom_parameters bp;
        bp.projected_element_count = 1000000;
        bp.false_positive_probability = 0.01;
        bp.compute_optimal_parameters();

        flow_count_t fca;
        flow_count_t fcl;

        fca.sources = new bloom_filter(bp);
        fcl.sources = new bloom_filter(bp);

        fca.count = fcl.count = 0;
       
        filters[bf_active].flows.push_back(fca);
        filters[bf_learning].flows.push_back(fcl);
    }
}

void clear_filters(flow_filter_t& filter_set)
{
    for (int i = 0;i < filter_set.flows.size(); i++) {
        filter_set.flows[i].sources->clear();
        filter_set.flows[i].count = 0;
    }
}

void destroy_filters(flow_filter_t* filters)
{
    for (int i = 0; i < filters[bf_active].flows.size(); i++) {
        delete filters[bf_active].flows[i].sources;
        delete filters[bf_learning].flows[i].sources;
    }
    filters[bf_active].flows.clear();
    filters[bf_active].flows.clear();
}

/*
 * Procedure for handling signals SIGTERM and SIGINT (Ctrl-C)
 */
void signal_handler(int signal)
{
    if (signal == SIGTERM || signal == SIGINT) {
        stop = 1;
        trap_terminate();
    }
}

// **********   BOGON PREFIX FILTER   **********
/**
 * Function for creating masks for IPv4 addresses.
 * Function fills the given array with every possible netmask for IPv4 address.
 * Size of this array is 33 items (see header file)
 *
 * @param m Array to be filled
 */
void create_v4_mask_map(ipv4_mask_map_t& m)
{
    m[0] = 0x00000000; // explicitly inserted or else it will be 0xFFFFFFFF
    for (int i = 1; i <= 32; i++) {
        m[i] = (0xFFFFFFFF >> (32 - i));
    }
}

/**
 * Function for creating masks for IPv6 addresses.
 * Functions fills the given array with every possible netmask for IPv6 address.
 * Size of the array is 129 items each containing 2 parts of IPv6 mask.
 *
 * @ param m Array to be filled
 */

void create_v6_mask_map(ipv6_mask_map_t& m)
{
    // explicitly inserted or else it will be 0xFF in every byte
    m[0][0] = m[0][1] = 0;

    for (int i = 1; i <= 128; i++) {
        if (i < 64) {
            m[i][0] = 0xFFFFFFFFFFFFFFFF >> (64 - i);
            m[i][1] = 0x0;
        } else {
            m[i][0] = 0xFFFFFFFFFFFFFFFF;
            m[i][1] = 0xFFFFFFFFFFFFFFFF >> (64 - i);
        }
    }
}

/*
 * Comparison functions for sorting the vector of loaded prefixes
 */
bool sort_by_prefix_v4 (const ip_prefix_t& addr1, const ip_prefix_t& addr2)
{
    return (memcmp(&(addr1.ip.ui32[2]), &(addr2.ip.ui32[2]), 4) < 0) ? true : false;
}

bool sort_by_prefix_v6 (const ip_prefix_t& addr1, const ip_prefix_t& addr2)
{
    return (memcmp(&addr1.ip.ui8, &addr2.ip.ui8, 16) < 0) ? true : false;
}

/**
 * Function for loading prefix file.
 * Function reads file with network prefixes and creates a vector for use
 * filters. This function should be called only once, since loading 
 * prefixes is needed only on "cold start of the detector" or if we want to 
 * teach the detector new file. (Possile changes to get signal for loading).
 *
 * @param prefix_list Reference to a structure for containing all prefixes
 * @return 0 if everything goes smoothly else 1
 */
int load_pref (pref_list_t& prefix_list_v4, pref_list_t& prefix_list_v6, const char *bogon_file)
{
    int error_cnt = 0;
    ip_prefix_t pref;
    ifstream pref_file;
    
    // open file with prefixes (hardcoded for now -- may be changed)
    pref_file.open(bogon_file);

    // unable to open prefix file
    if (!pref_file.is_open()) {
        cerr << "ERROR: File with bogon prefixes couldn't be loaded.";
        cerr << " Unable to continue." << endl;
        return BOGON_FILE_ERROR;
    }

    // loading the prefixes to memory
    while (!(pref_file.eof())) {

        // allocate memory for new item
        string raw_ip;
        getline(pref_file, raw_ip, '/');

        // trim whitespaces from the input
        raw_ip.erase(remove_if(raw_ip.begin(), raw_ip.end(), ::isspace), raw_ip.end());
        
        /*
         * Convert input to ip address for use in program
         * If it fails (invalid ip address) free the memory and continue 
         * to next line.
         */
        if (!ip_from_str(raw_ip.c_str(), &(pref.ip))) {
            continue;
        }
        // load prefix length (netmask
        getline(pref_file,raw_ip,'\n');

        // convert to number
        pref.pref_length = strtoul(raw_ip.c_str(), NULL, 0);

        if (ip_is4(&(pref.ip))) {
            prefix_list_v4.push_back(pref);
        } else {
            prefix_list_v6.push_back(pref);
        }

    }

    // sort lists for binary search used in filters
    sort(prefix_list_v4.begin(), prefix_list_v4.end(), sort_by_prefix_v4);
    sort(prefix_list_v6.begin(), prefix_list_v6.end(), sort_by_prefix_v6);

    pref_file.close();
    return ALL_OK;
}

/**
 * Filter for checking ipv4 for bogon prefixes.
 * This function checks the given ip address  whether it matches 
 * any of the bogon prefixes in list. If it does filter returns 
 * positive spoofing constant and spoofing counter is increased.
 * 
 * @param checked IP address that is being checked
 * @param prefix_list List of bogon prefixes used for checking
 * @param v4mm Array of every possible netmasks for protocol
 * @return SPOOF_POSITIVE if address fits the bogon prefix otherwise SPOOF_NEGATIVE
 */
int v4_bogon_filter(ur_basic_flow_t *checked, pref_list_t& prefix_list, ipv4_mask_map_t& v4mm)
{
    //check source address of the record with each prefix
    int begin, end, mid;
    int mask_result;
    ip_addr_t masked;
    begin = 0;
    end = prefix_list.size() - 1;
    while (begin <= end) {
        mid = (begin + end) >> 1;

        /* 
         * Matching address to prefix
         * Perform bitwise AND operation on integer value of ip address
         * and mask of the bogon prefix. Spoofing is positive if the result
         * of this operation is equal to integer value of bogon ip address.
         */
        masked.ui32[2] = checked->src_addr.ui32[2] & v4mm[prefix_list[mid].pref_length];

#ifdef DEBUG
        char debug_ip_src[INET6_ADDRSTRLEN];
        char debug_ip_pref[INET6_ADDRSTRLEN];
        ip_to_str(&(checked->src_addr), debug_ip_src);
        ip_to_str(&(prefix_list[mid].ip), debug_ip_pref);
#endif
        mask_result = memcmp(&(prefix_list[mid].ip.ui32[2]), &(masked.ui32[2]), 4);
        if (mask_result < 0) {
            begin = mid + 1;
        } else if (mask_result == 0) {

#ifdef DEBUG
            cout << "Possible spoofing found: ";
            cout << debug_ip_src;
            cout << " fits prefix ";
            cout << debug_ip_pref;
            cout <<"/";
            short a;
            cout << dec <<  (a = prefix_list[mid].pref_length) << endl;
#endif
            return SPOOF_POSITIVE;
        } else {
            end = mid - 1;
        }
    }

    // doesn't fit any bogon prefix
    return SPOOF_NEGATIVE;
}

/**
 * Filter for checking ipv6 for bogon prefixes.
 * This function checks the given ip address  whether it matches 
 * any of the bogon prefixes in list. If it does filter returns 
 * positive spoofing constant and spoofing counter is increased.
 * 
 * @param checked IP address that is being checked
 * @param prefix_list List of bogon prefixes used for checking
 * @param v4mm Array of every possible netmasks for protocol
 * @return SPOOF_POSITIVE if address fits the bogon prefix otherwise SPOOF_NEGATIVE
 */
int v6_bogon_filter(ur_basic_flow_t *checked, pref_list_t& prefix_list, ipv6_mask_map_t& v6mm)
{

    int begin, end, mid;
    int mask_result;
    ip_addr_t masked;
    begin = 0;
    end = prefix_list.size() - 1;

    while (begin <= end) {
    mid = (begin + end) >> 1;
#ifdef DEBUG
        char debug_ip_src[INET6_ADDRSTRLEN];
        char debug_ip_pref[INET6_ADDRSTRLEN];
        ip_to_str(&(checked->src_addr), debug_ip_src);
        ip_to_str(&(prefix_list[mid].ip), debug_ip_pref);

#endif
        
        /* 
         * Matching address to prefix
         * We can decide which part to AND with bogon prefix by the length.
         * If the length of the bogon prefix is shorter or equal to 64 bits
         * we need to bitwise AND only the first half of the ip addre 
         * and then compare it to the value of the first half of the bogon
         * prefix. If it's longer then we AND the second half of the ip address 
         * and then we compare the whole result with the bogon prefix. Spoofing 
         * is positive when the result of comparison fits the prefix.
         */

        if (prefix_list[mid].pref_length <= 64) {
            masked.ui64[0] = checked->src_addr.ui64[0] & v6mm[prefix_list[mid].pref_length][0];
            mask_result = memcmp(&(prefix_list[mid].ip.ui64[0]), &(masked.ui64[0]), 8);
        } else {
            masked.ui64[1] = checked->src_addr.ui64[1] & v6mm[prefix_list[mid].pref_length][1];
            mask_result = memcmp(&(prefix_list[mid].ip.ui8), &(masked.ui8), 16);
        } 
        if (mask_result < 0) {
            begin = mid + 1;
        } else if (mask_result == 0) {
#ifdef DEBUG
            cout << "Possible spoofing found: ";
            cout << debug_ip_src;
            cout << " fits bogon prefix ";
            cout << debug_ip_pref;
            cout <<"/";
            short a;
            cout << dec <<  (a = prefix_list[mid].pref_length) << endl;
#endif
            return SPOOF_POSITIVE;
        } else {
            end = mid - 1;
        }
    }
    return SPOOF_NEGATIVE;   
}

// **********   SYMETRIC ROUTING FILTER   **********

/**
 * Function for checking routing symetry for IPv4.
 * Function takes the direction flag from the record and based on its value 
 * it decides whether to associate the link with its source IP or to check 
 * the link used by the communication. Checking of the communication is 
 * done based on the map of links their respective source IP address. The map 
 * is filled from outgoing communication by the destination address. If the 
 * communication flow is incomming then the source address is used as a key 
 * to the map to get the link used by this communiation. If the link fits 
 * the bitmask stored on this location then the communication is considered 
 * legit (the route is symetric). If the result of masking  AND operation is
 * 0x0 then there is no valid link for this communication and the source IP is 
 * flagged as spoofed.
 *
 * @param record Record (unirec format) that is being analyzed.
 * @param src Map with link masks associated to their respective sources.
 * @param rw_time Time before updating (rewriting) the link record in the map.
 * @return SPOOF_NEGATIVE if the route is symetric otherwise SPOOF_POSITIVE.
 */

int check_symetry_v4(ur_basic_flow_t *record, v4_sym_sources_t& src, unsigned rw_time)
{

#ifdef DEBUG
    char debug_ip_src[INET6_ADDRSTRLEN];
    char debug_ip_dst[INET6_ADDRSTRLEN];
    ip_to_str(&(record->src_addr), debug_ip_src);
    ip_to_str(&(record->dst_addr), debug_ip_dst);
#endif


    unsigned v4_numeric;

    // check incomming/outgoing traffic
    if (record->dirbitfield == 0x0) {// outgoing trafic
        // mask with 24-bit long prefix
        v4_numeric = ip_get_v4_as_int(&(record->dst_addr)) & 0xFFFFFF00;

        if (src.count(v4_numeric)
            && (((record->first & 0xFFFFFFFF00000000) - src[v4_numeric].timestamp) < rw_time)) {
#ifdef DEBUG
            cout <<(unsigned long long) (((record->first & 0xFFFFFFFF00000000) - src[v4_numeric].timestamp) < rw_time) << endl;
#endif
            src[v4_numeric].link |= record->linkbitfield;
            src[v4_numeric].timestamp = record->first;
        } else {
            sym_src_t src_rec;
            src_rec.link = record->linkbitfield;
            src_rec.timestamp = record->first;
            src.insert(pair<int, sym_src_t>(v4_numeric, src_rec));
        }

    } else { // incomming traffic --> check for validity
        // mask with 24-bit long prefix
        v4_numeric = ip_get_v4_as_int(&(record->src_addr)) & 0xFFFFFF00;
        if (src.count(v4_numeric)) {
            int valid = src[v4_numeric].link & record->linkbitfield;
            if (valid == 0x0) {
                //no valid link found => possible spoofing
#ifdef  DEBUG
                cout << debug_ip_src << " ---> " << debug_ip_dst << endl;
                cout << "Flow goes through " << (long long) record->linkbitfield << " while stored is " << (long long) src[v4_numeric].link  << endl;
                cout << "Possible spoofing found: tested route is asymetric." << endl;
#endif
                return SPOOF_POSITIVE;
            } else {
                // trafic went through the valid link
                return SPOOF_NEGATIVE;
            }
        } else { // no bit record found -- can't decide
            return SPOOF_NEGATIVE;
        }
    }
    return SPOOF_NEGATIVE;
}

/**
 * Function for checking routing symetry for IPv6.
 * Function takes the direction flag from the record and based on its value 
 * it decides whether to associate the link with its source IP or to check 
 * the link used by the communication. Checking of the communication is 
 * done based on the map of links their respective source IP address. The map 
 * is filled from outgoing communication by the destination address. If the 
 * communication flow is incomming then the source address is used as a key 
 * to the map to get the link used by this communiation. If the link fits 
 * the bitmask stored on this location then the communication is considered 
 * legit (the route is symetric). If the result of masking  AND operation is
 * 0x0 then there is no valid link for this communication and the source IP is 
 * flagged as spoofed.
 *
 * @param record Record (unirec format) that is being analyzed.
 * @param src Map with link masks associated to their respective sources.
 * @param rw_time Time before updating (rewriting) the link record in the map.
 * @return SPOOF_NEGATIVE if the route is symetric otherwise SPOOF_POSITIVE.
 */


int check_symetry_v6(ur_basic_flow_t *record, v6_sym_sources_t& src, unsigned rw_time)
{

#ifdef DEBUG
    char debug_ip_src[INET6_ADDRSTRLEN];
    char debug_ip_dst[INET6_ADDRSTRLEN];
    ip_to_str(&(record->src_addr), debug_ip_src);
    ip_to_str(&(record->dst_addr), debug_ip_dst);
#endif

    //  Swap the halves of the addresses again
    //  No idea why the address from recieved record is messed up
    record->src_addr = ip_from_16_bytes_le((char *) &(record->src_addr));
    record->dst_addr = ip_from_16_bytes_le((char *) &(record->dst_addr));

    uint64_t tmp;
    tmp = record->src_addr.ui64[1];
    record->src_addr.ui64[1] = record->src_addr.ui64[0];
    record->src_addr.ui64[0] = tmp;

    tmp = record->dst_addr.ui64[1];
    record->dst_addr.ui64[1] = record->dst_addr.ui64[0];
    record->dst_addr.ui64[0] = tmp;

    // check incomming/outgoing traffic
    if (record->dirbitfield == 0x0) {// outgoing traffic
        // for future use with /48 prefix length
        // record->dst_addr.ui64[0] &= 0xFFFFFFFFFFFF0000;

        if (src.count(record->dst_addr.ui64[0])
            && ((record->first & 0xFFFFFFFF00000000) - src[record->dst_addr.ui64[0]].timestamp) < rw_time) {
            src[record->dst_addr.ui64[0]].link |= record->linkbitfield;
//            src[record->dst_addr.ui64[0]].timestamp = "timestamp from unirec"
        } else {
            sym_src_t src_rec;
            src_rec.link = record->linkbitfield;
//            src_rec.timestamp = "timestamp from unirec"
            src.insert(pair<uint64_t, sym_src_t>(record->dst_addr.ui64[0], src_rec));
        }

    } else { // incomming traffic --> check for validity
        // for future use with /48 prefix length
        //record->src_addr.ui64[0] &= 0xFFFFFFFFFFFF0000;

        if (src.count(record->src_addr.ui64[0])) {
            int valid = src[record->src_addr.ui64[0]].link & record->linkbitfield;
            if (valid == 0x0) {
                //no valid link found => possible spoofing
#ifdef  DEBUG
                cout << debug_ip_src << " ---> " << debug_ip_dst << endl;
                cout << "Flow goes through " << (long long) record->linkbitfield << " while stored is " << (long long) src[record->src_addr.ui64[0]].link  << endl;
                cout << "Possible spoofing found: tested route is asymetric." << endl;
#endif
                return SPOOF_POSITIVE;
            } else {
                return SPOOF_NEGATIVE;
            }
        } else { // no bit record found
            return SPOOF_NEGATIVE;
        }
    }
    return SPOOF_NEGATIVE;
}

// **********   NEW FLOW COUNT FILTER   **********

/**
 * Function for cheking new flows for given source (IPv4).
 * Function gets the record and map of used data flows. Then it asks
 * the map for source source address. If the source is not present it adds the 
 * new source with its destination and initializes its counter to 1. If the 
 * source already communicated then the destination is added to the set of flows 
 * and the counter is increased. If the flow count exceeds the given threshold 
 * the source address is reported as spoofed.
 *
 * @param record Record that is being analyzed.
 * @param flow_map Map of all used flows.
 * @param threshold Maximum limit for flows per source.
 * @return SPOOF_POSITIVE if the flow count exceeds the threshold.
 */
int check_new_flows_v4(ur_basic_flow_t *record, unsigned threshold, flow_filter_t* filter, ipv4_mask_map_t& mm, pref_list_t& prefix_list)
{

#ifdef DEBUG
    char debug_ip_src[INET6_ADDRSTRLEN];
    char debug_ip_dst[INET6_ADDRSTRLEN];
    ip_to_str(&(record->src_addr), debug_ip_src);
    ip_to_str(&(record->dst_addr), debug_ip_dst);
#endif

    // check the timestamp of filters
    uint64_t tf, tr;

    tf = filter[bf_active].timestamp & 0xFFFFFFFF00000000;
    tr = record->first & 0xFFFFFFFF00000000;


    /*
     * Filter swapping will be implemented after the timestamps are ready to use
     */

    /*
    if ((tf - tr) > BF_SWAP_TIME) {
        swap_filters();
        clear_filters(filter[bf_learning]);
        filter[bf_active].timestamp = record->first;
        filter[bf_learning].timestamp = record->first;
    }*/

    char ip_key[INET6_ADDRSTRLEN];
    bool is_present = false;

    //test for cesnet or other specified prefixes
    
    int begin, end, mid;
    ip_addr_t masked;
    int search_result;
    begin = 0;
    end = prefix_list.size() - 1;

    while (begin <= end) {
        mid = (begin + end) >> 1; // division by 2
        masked.ui32[2] = record->dst_addr.ui32[2] & mm[prefix_list[mid].pref_length];
        search_result = memcmp(&(prefix_list[mid].ip.ui32[2]), &(masked.ui32[2]), 4);
        
        if (search_result < 0) {
            begin = mid + 1;
        } else if (search_result > 0) {
            end = mid -1;
        } else {
            break;
        }
    }

    // Source address doesn't fit the watched networks --> ignored
    if (search_result != 0) {
        return SPOOF_NEGATIVE;
    }

    masked = record->src_addr;
    masked.ui32[2] &= mm[24]; // mask with 24-bit prefix for aggregation

    // convert to BF key
    ip_to_str(&(masked), ip_key);

    //  test if the flow is present (BF)
    is_present = filter[bf_active].flows[mid].sources->contains((unsigned char *) ip_key, INET6_ADDRSTRLEN);

    if (is_present) { // the flow is already in filter --> will be ignored
#ifdef DEBUG
        cout << "Flow in filter --> ignore ... " << endl;
#endif
        return SPOOF_NEGATIVE;
    } else {
        // insert to both filters and increase their respective counts
        filter[bf_active].flows[mid].sources->insert(ip_key, INET6_ADDRSTRLEN);
        filter[bf_learning].flows[mid].sources->insert(ip_key, INET6_ADDRSTRLEN);
        filter[bf_active].flows[mid].count++;
        filter[bf_learning].flows[mid].count++;

        if (filter[bf_active].flows[mid].count > threshold) {
        // Flow limit exceeded
#ifdef DEBUG
            cout << "Possible spoofing found: ";
            cout << debug_ip_dst << " recieving too many flows (" << filter[bf_active].flows[mid].count << ")." << endl;
#endif
            return SPOOF_POSITIVE;
        }
    }
    return SPOOF_NEGATIVE;
}

/**
 * Function for cheking new flows for given source (IPv6).
 * Function gets the record and map of used data flows. Then it asks
 * the map for source source address. If the source is not present it adds the 
 * new source with its destination and initializes its counter to 1. If the 
 * source already communicated then the destination is added to the set of flows 
 * and the counter is increased. If the flow count exceeds the given threshold 
 * the source address is reported as spoofed.
 *
 * @param record Record that is being analyzed.
 * @param flow_map Map of all used flows.
 * @param threshold Maximum limit for flows per source.
 * @return SPOOF_POSITIVE if the flow count exceeds the threshold.
 */
int check_new_flows_v6(ur_basic_flow_t *record, unsigned threshold, flow_filter_t* filter, ipv6_mask_map_t& mm, pref_list_t& prefix_list)
{

#ifdef DEBUG
    char debug_ip_src[INET6_ADDRSTRLEN];
    char debug_ip_dst[INET6_ADDRSTRLEN];
    ip_to_str(&(record->src_addr), debug_ip_src);
    ip_to_str(&(record->dst_addr), debug_ip_dst);
#endif

    // check the timestamp of filters
    uint64_t tf, tr;

    tf = filter[bf_active].timestamp & 0xFFFFFFFF00000000;
    tr = record->first & 0xFFFFFFFF00000000;


    /*
     * Filter swapping will be implemented after the timestamps are ready to use
     */

    /*
    if ((tf - tr) > BF_SWAP_TIME) {
        swap_filters();
        clear_filters(filter[bf_learning]);
        filter[bf_active].timestamp = record->first;
        filter[bf_learning].timestamp = record->first;
    }*/

    char ip_key[INET6_ADDRSTRLEN];
    bool is_present = false;

    //test for cesnet or other specified prefixes
    
    int begin, end, mid;
    ip_addr_t masked;
    int search_result;
    begin = 0;
    end = prefix_list.size() - 1;

    while (begin <= end) {
        mid = (begin + end) >> 1; // division by 2
        if (prefix_list[mid].pref_length > 64) {
            masked.ui64[0] = record->src_addr.ui64[0] & mm[prefix_list[mid].pref_length][0];
            search_result = memcmp(&(prefix_list[mid].ip.ui64[0]), &(masked.ui64[0]), 8);
        } else {
            masked.ui64[1] = record->src_addr.ui64[1] & mm[prefix_list[mid].pref_length][1];
            search_result = memcmp(&(prefix_list[mid].ip.ui8), &(masked.ui8), 16);
        }
        
        if (search_result < 0) {
            begin = mid + 1;
        } else if (search_result > 0) {
            end = mid -1;
        } else {
            break;
        }
    }


    if (search_result != 0) {
        return SPOOF_NEGATIVE;
    }

    masked = record->src_addr;
    masked.ui64[0] &= mm[64][0]; // mask with 64-bit prefix for aggregation
    masked.ui64[1] &= 0x0;

    // convert to BF key
    ip_to_str(&(masked), ip_key);

    //  test if the flow is present (BF)
    is_present = filter[bf_active].flows[mid].sources->contains((unsigned char *) ip_key, INET6_ADDRSTRLEN);

    if (is_present) {
        return SPOOF_NEGATIVE;
    } else {
        // insert to both filters and increase their respective counts
        filter[bf_active].flows[mid].sources->insert(ip_key, INET6_ADDRSTRLEN);
        filter[bf_learning].flows[mid].sources->insert(ip_key, INET6_ADDRSTRLEN);
        filter[bf_active].flows[mid].count++;
        filter[bf_learning].flows[mid].count++;

        if (filter[bf_active].flows[mid].count > threshold) {
#ifdef DEBUG
            cout << "Possible spoofing found: ";
            cout << debug_ip_dst << " recieving too many flows (" << filter[bf_active].flows[mid].count << ")." << endl;
#endif
            return SPOOF_POSITIVE;
        }
    }
    return SPOOF_NEGATIVE;
}

/*
 * MAIN FUNCTION
 */
int main (int argc, char** argv)
{

    int retval = 0; // return value

    trap_ifc_spec_t ifc_spec; // interface specification for TRAP

    ur_basic_flow_t *record; // pointer on flow record

    // lists of bogon prefixes
    pref_list_t bogon_list_v4; 
    pref_list_t bogon_list_v6;
    pref_list_t spec_list_v4;
    pref_list_t spec_list_v6;

    ipv4_mask_map_t v4_masks; // all possible IPv4 masks
    ipv6_mask_map_t v6_masks; // all possible IPv6 masks

    v4_sym_sources_t v4_route_sym; // map of sources for symetric routes (IPv4)
    v6_sym_sources_t v6_route_sym; // map of sources for symetric routes (IPv6)

    flow_filter_t v4_flows[2]; // Bloom filter structures for new flow filter (IPv4)
    flow_filter_t v6_flows[2]; // Bloom filter structures for new flow filter (IPv6)

    // Initialize TRAP library (create and init all interfaces)
    retval = trap_parse_params(&argc, argv, &ifc_spec);
    if (retval != TRAP_E_OK) {
        if (retval == TRAP_E_HELP) {
            trap_print_help(&module_info);
            return EXIT_SUCCESS;
        }
        cerr << "ERROR: Cannot parse input parameters: " << trap_last_error_msg << endl;
        return retval;
    }
     
    // getopt loop for additional parameters not parsed by TRAP
    int argret = 0;
    unsigned sym_rw_time = 0;
    unsigned nf_threshold = 0;
    bool b_flag = false;
    string bog_filename;
    string cnet_filename;

    while ((argret = getopt(argc, argv, "b:c:s:t:")) != -1) {
        switch (argret) {
            case 'b':
                bog_filename = string(optarg);
                b_flag = true;
                break;
            
            case 'c':
                cnet_filename = string(optarg);
                break;

            case 's':
                sym_rw_time = atoi(optarg);
                break;

            case 't':
                nf_threshold = atoi(optarg);
                break;
            
            case '?':
                if (argret == 'b') {
                    cerr << "Option -b requires an argument -- file with bogon prefixes." << endl;
                    return EXIT_FAILURE;
                } else if (argret == 't') {
                    cerr << "Option -t requires an argument -- number of maximum new flows per source." << endl;
                    return EXIT_FAILURE;
                }
                break;
        }
    }

    if (! b_flag) {
        cerr << "ERROR: Bogon file not specified. Unable to continue." << endl;
        return EXIT_FAILURE;
    }

#ifdef DEBUG
    if (sym_rw_time == 0) {
        cout << "Symetric filter update time not specified. Default time will be used instead." << endl;    
    }
    if (nf_threshold == 0) {
        cout << "New flow threshold set to 0. Default will be used instead." << endl;
    }
#endif

    // set the default rw_time
    if (sym_rw_time == 0) {
        sym_rw_time = SYM_RW_DEFAULT;
    }
    if (nf_threshold == 0) {
        nf_threshold = NEW_FLOW_DEFAULT;
    }

    retval = trap_init(&module_info, ifc_spec);
    if (retval != TRAP_E_OK) {
        cerr << "ERROR: TRAP couldn't be initialized: " << trap_last_error_msg << endl;
        return retval;
    }
    // free interface specification structure
    trap_free_ifc_spec(ifc_spec);

    // set signal handling for termination
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // fill the netmask masks
    create_v4_mask_map(v4_masks);    
    create_v6_mask_map(v6_masks);

#ifdef DEBUG
    unsigned v4 = 0;
    unsigned v6 = 0;
    unsigned spoof_count = 0;
    unsigned bogons = 0;
    unsigned syms = 0;
    unsigned nflows = 0;
#endif

    // we don't have list of bogon prefixes loaded (usually first run)
    retval = load_pref(bogon_list_v4, bogon_list_v6, bog_filename.c_str());
    retval = load_pref(spec_list_v4, spec_list_v6, cnet_filename.c_str());

    if (retval == BOGON_FILE_ERROR) {
        return retval;
    }

    // create Bloom filters
    create_nflow_filters(spec_list_v4.size(), v4_flows);
    create_nflow_filters(spec_list_v6.size(), v6_flows);

#ifdef DEBUG
    cout << "Bloom filters created. " << endl;
#endif

    // ***** Main processing loop *****
    while (!stop) {
        const void *data;
        uint16_t data_size;
                
        // retrieve data from server
        retval = trap_get_data(TRAP_MASK_ALL, &data, &data_size, TRAP_WAIT);
        if (retval != TRAP_E_OK) {
            if (retval == TRAP_E_TERMINATED) { // trap is terminated
                break;
            } else { // recieve error
                cerr << "ERROR: Unable to get data. Return value ";
                cerr << dec << retval;
                cerr << " (" << trap_last_error_msg << ")." <<  endl;
                break;
            }
        }

        // check the data size 
        if (data_size != sizeof(ur_basic_flow_t)) {
            if (data_size <= 1) { // end of data
                break;
            } else { // data corrupted
                cerr << "ERROR: Wrong data size. ";
                cerr << "Expected: " << sizeof(ur_basic_flow_t) << " ";
                cerr << "Recieved: " << data_size << endl;
                break;
            }
        }

        // Interpret data as unirec flow record
        record = (ur_basic_flow_t *) data;

        if (ip_is4(&(record->src_addr))) {
            ++v4;
        } else {
            ++v6;
        }

        //go through all filters
        

        // ***** 1. bogon and specific prefix filter *****
        if (ip_is4(&(record->src_addr))) {
            retval = v4_bogon_filter(record, bogon_list_v4, v4_masks);
            if (retval == SPOOF_NEGATIVE && record->dirbitfield == 0x01) {
                retval = v4_bogon_filter(record, spec_list_v4, v4_masks);
            }
        } else {
            retval = v6_bogon_filter(record, bogon_list_v6, v6_masks);
            if (retval == SPOOF_NEGATIVE && record->dirbitfield == 0x01) {
                retval = v6_bogon_filter(record, spec_list_v6, v6_masks);
            }
        }
       
        // we caught a spoofed address by bogon prefix
        if (retval == SPOOF_POSITIVE) {
#ifdef DEBUG
            ++spoof_count;
            ++bogons;
#endif
            //for future use
            trap_send_data(1, record, sizeof(ur_basic_flow_t), TRAP_WAIT);
            retval = ALL_OK; // reset return value
            continue;
        }
        // ***** 2. symetric routing filter *****
        if (ip_is4(&(record->src_addr))) {
            retval = check_symetry_v4(record, v4_route_sym, sym_rw_time);
        } else {
            retval = check_symetry_v6(record, v6_route_sym, sym_rw_time);
        }
        
        // we caught a spoofed address by not keeping to symteric routing
        if (retval == SPOOF_POSITIVE) {
#ifdef DEBUG
            ++spoof_count;
            ++syms;
#endif
            //for future use
            trap_send_data(1, record, sizeof(ur_basic_flow_t), TRAP_WAIT);
            retval = ALL_OK;
            continue;
        }
        
        //3. asymetric routing filter (will be implemented later)
        //4. new flow count check (TBA)

        if (ip_is4(&(record->src_addr))) {
            retval = check_new_flows_v4(record, nf_threshold, v4_flows, v4_masks, spec_list_v4); 
        } else {
            retval = check_new_flows_v6(record, nf_threshold, v6_flows, v6_masks, spec_list_v6);
        }
    
        if (retval == SPOOF_POSITIVE) {
#ifdef DEBUG
            ++spoof_count;
            ++nflows;
#endif
            //for future use
            trap_send_data(1, record, sizeof(ur_basic_flow_t), TRAP_WAIT);
            retval = ALL_OK;
            continue;
        }
    }

#ifdef DEBUG
    cout << "IPv4: ";
    cout << dec << v4 << endl;

    cout << "IPv6: ";
    cout << dec << v6 << endl;
    cout << "No. of possibly spoofed addresses: ";
    cout << dec << spoof_count << endl;
    cout << "Caught by bogon filter: " << bogons << endl;
    cout << "Caught by symetric routing filter: " << syms << endl;
    cout << "Caught by using too many new flows: " << nflows << endl;
#endif

    // clean up before termination
    destroy_filters(v4_flows);
    destroy_filters(v6_flows);
    trap_finalize();

    return EXIT_SUCCESS;
}
