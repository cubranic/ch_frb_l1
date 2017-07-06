// Major features missing:
//
//   - RFI removal is a placeholder
//   - Alex Josephy's grouping/sifting code is not integrated
//   - Distributed logging is not integrated
//
// Currently hardcoded to assume the NUMA setup of the CHIMEFRB L1 nodes:
//   - Dual CPU
//   - 10 cores/cpu
//   - Hyperthreading enabled
//   - all NIC's on the same PCI-E bus as the first CPU.
//
// Note that the Linux scheduler defines 40 "cores":
//   cores 0-9:    primary hyperthread on CPU1 
//   cores 10-19:  primary hyperthread on CPU2
//   cores 20-29:  secondary hyperthread on CPU1
//   cores 30-39:  secondary hyperthread on CPU2

#include <thread>
#include <fstream>

#include <ch_frb_io.hpp>
#include <rf_pipelines.hpp>
#include <bonsai.hpp>
#include <l1-rpc.hpp>

#include "ch_frb_l1.hpp"
#include "chlog.hpp"

using namespace std;
using namespace ch_frb_l1;


static void usage()
{
    cerr << "Usage: ch-frb-l1 [-v] <l1_config.yaml> <rfi_config.txt> <bonsai_config.txt> <l1b_config_file>\n"
	 << "  The -v flag makes the output more verbose\n";
    exit(2);
}


// -------------------------------------------------------------------------------------------------


struct l1_params {
    l1_params(int argc, char **argv);

    string l1_config_filename;
    string rfi_config_filename;
    string bonsai_config_filename;
    string l1b_config_filename;

    Json::Value rfi_transform_chain_json;
    bonsai::config_params bonsai_config;

    // Current values:
    //   verbosity=1: pretty quiet
    //   verbosity=2: pretty noisy
    // I'll probably add more values later!
    int verbosity = 1;

    // nstreams is automatically determined by the number of (ipaddr, port) pairs.
    // There will be one (network_thread, assembler_thread, rpc_server) triple for each stream.
    int nbeams = 0;
    int nstreams = 0;

    // The L1 server can run in two modes: either a "full-scale" mode with 16 beams and 20 cores,
    // or a "subscale" mode with (nbeams <= 4) and no core-pinning.
    bool is_subscale = true;
    
    // If slow_kernels=false (the default), the L1 server will use fast assembly language kernels
    // for its packet processing.  If slow_kernels=true, then it will use reference kernels which
    // are much slower.
    //
    // Note 1: the slow kernels are too slow for non-subscale use!  If slow kernels are used on
    // the "full-scale" L1 server with (nbeams, nupfreq) = (16, 16), it may crash.
    //
    // Note 2: the fast kernels can only be used if certain conditions are met.  As of this writing,
    // the conditions are: (a) nupfreq must be even, and (b) nt_per_packet must be 16.  In particular,
    // for a subscale run with nupfreq=1, the fast kernels can't be used.
    //
    // Conditions (a) and (b) could be generalized by writing a little more code, if this would be useful
    // then let me know!
    bool slow_kernels = false;

    // Both vectors have length nstreams.
    vector<string> ipaddr;
    vector<int> port;

    // One L1-RPC per stream
    vector<string> rpc_address;

    // L1b linkage.
    // Note: assumed L1b command line is:
    //   <l1_executable_filename> <l1b_config> <beam_id>

    std::string l1b_executable_filename;
    bool l1b_pipe_blocking = true;
    bool l1b_search_path = false;     // will $PATH be searched for executable?
    int l1b_pipe_capacity = 0;        // zero means "system default"
};


l1_params::l1_params(int argc, char **argv)
{
    vector<string> args;

    // Low-budget command line parsing

    for (int i = 1; i < argc; i++) {
	if (!strcmp(argv[i], "-v"))
	    this->verbosity = 2;
	else
	    args.push_back(argv[i]);
    }

    if (args.size() != 4)
	usage();

    this->l1_config_filename = args[0];
    this->rfi_config_filename = args[1];
    this->bonsai_config_filename = args[2];
    this->l1b_config_filename = args[3];

    // Read rfi_config file.
    std::ifstream rfi_config_file(rfi_config_filename);
    if (rfi_config_file.fail())
        throw runtime_error("ch-frb-l1: couldn't open file " + rfi_config_filename);

    Json::Reader rfi_config_reader;
    if (!rfi_config_reader.parse(rfi_config_file, this->rfi_transform_chain_json))
	throw runtime_error("ch-frb-l1: couldn't parse json file " + rfi_config_filename);

    // Throwaway call, to get an early check that rfi_config_file is valid.
    rf_pipelines::deserialize_transform_chain_from_json(this->rfi_transform_chain_json);

    // Read bonsai_config file.
    this->bonsai_config = bonsai::config_params(bonsai_config_filename);

    // Remaining code in this function reads l1_config file.
    yaml_paramfile p(l1_config_filename);

    this->nbeams = p.read_scalar<int> ("nbeams");
    this->ipaddr = p.read_vector<string> ("ipaddr");
    this->port = p.read_vector<int> ("port");
    this->rpc_address = p.read_vector<string> ("rpc_address");
    this->slow_kernels = p.read_scalar<bool> ("slow_kernels", false);
    this->l1b_executable_filename = p.read_scalar<string> ("l1b_executable_filename");
    this->l1b_search_path = p.read_scalar<bool> ("l1b_search_path", false);
    this->l1b_pipe_capacity = p.read_scalar<int> ("l1b_pipe_capacity", 0);
    this->l1b_pipe_blocking = p.read_scalar<bool> ("l1b_pipe_blocking", true);

    // 2 * (number of threads), where factor 2 is from hyperthreading.
    int hwcon = std::thread::hardware_concurrency();

    if (nbeams <= 4)
	this->is_subscale = true;
    else if ((nbeams == 16) && (hwcon == 40))
	this->is_subscale = false;
    else {
	cerr << "ch-frb-l1: The L1 server can currently run in two modes: either a \"full-scale\" mode\n"
	     << "  with 16 beams and 20 cores, or a \"subscale\" mode with 4 beams and no core-pinning.\n"
	     << "  This appears to be an instance with " << nbeams << " beams, and " << (hwcon/2) << " cores.\n";
	exit(1);
    }

    if ((ipaddr.size() == 1) && (port.size() > 1))
	this->ipaddr = vector<string> (port.size(), ipaddr[0]);
    else if ((ipaddr.size() > 1) && (port.size() == 1))
	this->port = vector<int> (ipaddr.size(), port[0]);
    
    if (ipaddr.size() != port.size())
	throw runtime_error(l1_config_filename + " expected 'ip_addr' and 'port' to be lists of equal length");
    
    this->nstreams = ipaddr.size();

    assert(nbeams > 0);
    assert(nstreams > 0);
    assert(ipaddr.size() == (unsigned int)nstreams);
    assert(port.size() == (unsigned int)nstreams);
    assert(rpc_address.size() == (unsigned int)nstreams);

    if (nbeams % nstreams) {
	throw runtime_error(l1_config_filename + " nbeams (=" + to_string(nbeams) + ") must be a multiple of nstreams (="
			    + to_string(nstreams) + ", inferred from number of (ipaddr,port) pairs");
    }

    p.check_for_unused_params();
}


// -------------------------------------------------------------------------------------------------
//
// make_input_stream(): returns a stream object which will read packets from the correlator.


static shared_ptr<ch_frb_io::intensity_network_stream> make_input_stream(const l1_params &config, int istream)
{
    assert(istream >= 0 && istream < config.nstreams);

    int nbeams = config.nbeams;
    int nstreams = config.nstreams;
    int nbeams_per_stream = xdiv(nbeams, nstreams);
    
    ch_frb_io::intensity_network_stream::initializer ini_params;

    ini_params.ipaddr = config.ipaddr[istream];
    ini_params.udp_port = config.port[istream];
    ini_params.beam_ids = vrange(istream * nbeams_per_stream, (istream+1) * nbeams_per_stream);
    ini_params.mandate_fast_kernels = !config.slow_kernels;
    ini_params.mandate_reference_kernels = config.slow_kernels;
    
    // Setting this flag means that an exception will be thrown if either:
    //
    //    1. the unassembled-packet ring buffer between the network and
    //       assembler threads is full (i.e. assembler thread is running slow)
    //
    //    2. the assembled_chunk ring buffer between the network and
    //       processing threads is full (i.e. processing thread is running slow)
    //
    // If we wanted, we could define separate flags for these two conditions.
    //
    // Note that in situation (2), the pipeline will crash anyway since
    // rf_pipelines doesn't contain code to handle gaps in the data.  This
    // is something that we'll fix soon, but it's nontrivial.
    
    ini_params.throw_exception_on_buffer_drop = true;

    // This disables the "telescoping" part of the telescoping ring buffers.
    // Currently, the telescoping logic is too slow for real-time use.  (The
    // symptom is that the assembler threads run slow, triggering condition (1)
    // from the previous comment.)  We should be able to fix this by writing
    // fancy assembly language kernels for the telescoping logic!

    ini_params.assembled_ringbuf_nlevels = 1;

    if (!config.is_subscale) {
	// Core-pinning logic for the full-scale L1 server.

	if (nstreams % 2 == 1)
	    throw runtime_error("ch-frb-l1: nstreams must be even, in order to divide dedispersion threads evenly between the two CPUs");

	// Note that processing threads 0-7 are pinned to cores 0-7 (on CPU1)
	// and cores 10-17 (on CPU2).  I decided to pin assembler threads to
	// cores 8 and 18.  This leaves cores 9 and 19 free for RPC and other IO.
	
	if (istream < (nstreams/2))
	    ini_params.assembler_thread_cores = {8,28};
	else
	    ini_params.assembler_thread_cores = {18,38};

	// I decided to pin all network threads to CPU1, since according to
	// the motherboard manual, all NIC's live on the same PCI-E bus as CPU1.
	//
	// I think it makes sense to avoid pinning network threads to specific
	// cores on the CPU, since they use minimal cycles, but scheduling latency
	// is important for minimizing packet drops.  I haven't really tested this
	// assumption though!

	ini_params.network_thread_cores = vconcat(vrange(0,10), vrange(20,30));
    }

    return ch_frb_io::intensity_network_stream::make(ini_params);
}


// -------------------------------------------------------------------------------------------------
//
// make_l1rpc_server()


static shared_ptr<L1RpcServer> make_l1rpc_server(const l1_params &config, int istream, shared_ptr<ch_frb_io::intensity_network_stream> stream) 
{
    assert(istream >= 0 && istream < config.nstreams);

    shared_ptr<L1RpcServer> rpc = make_shared<L1RpcServer>(stream, config.rpc_address[istream]);
    return rpc;
}


// -------------------------------------------------------------------------------------------------


static void dedispersion_thread_main(const l1_params &config, const shared_ptr<ch_frb_io::intensity_network_stream> &sp, int ibeam)
{
    try {
	vector<int> allowed_cores;

	if (!config.is_subscale) {
	    // Core-pinning logic for full-scale L1 server.
	    int c = (ibeam / 8) * 10 + (ibeam % 8);
	    allowed_cores = { c, c+20 };
	}

	// Pin thread before allocating anything.
	// Note that in the subscale case, 'allowed_cores' is an empty vector, and pin_thread_to_cores() no-ops.
	ch_frb_io::pin_thread_to_cores(allowed_cores);
	
        auto stream = rf_pipelines::make_chime_network_stream(sp, ibeam);
	auto transform_chain = rf_pipelines::deserialize_transform_chain_from_json(config.rfi_transform_chain_json);

	bonsai::dedisperser::initializer ini_params;
	ini_params.verbosity = 0;

	auto dedisperser = make_shared<bonsai::dedisperser> (config.bonsai_config, ini_params);

	if (config.l1b_executable_filename.size() > 0) {
	    // Assumed L1b command line is: <l1_executable> <l1b_config> <beam_id>
	    vector<string> l1b_command_line = {
		config.l1b_executable_filename,
		config.l1b_config_filename,
		std::to_string(ibeam)
	    };

	    bonsai::trigger_pipe::initializer l1b_pipe_ini;
	    l1b_pipe_ini.pipe_capacity = config.l1b_pipe_capacity;
	    l1b_pipe_ini.blocking = config.l1b_pipe_blocking;
	    l1b_pipe_ini.search_path = config.l1b_search_path;

	    // Important: pin L1b child process to same core as L1a parent thread.
	    // Note that in the subscale case, 'allowed_cores' is an empty vector, and the child process is not core-pinned.
	    l1b_pipe_ini.child_cores = allowed_cores;

	    // The trigger_pipe constructor will spawn the L1B child process.
	    auto tp = make_shared<bonsai::trigger_pipe> (l1b_command_line, l1b_pipe_ini);
	    dedisperser->add_processor(tp);
	}
	else
	    cout << "ch-frb-l1: config parameter 'l1b_executable_filename' is an empty string, L1B processes will not be spawned\n";

	// During development, it's convenient to throw in a bonsai::global_max_tracker,
	// so that the dedispersion thread can print the most significant (DM, arrival_time)
	// when it exits.
	//
	// FIXME: eventually the global_max_tracker can be removed (it won't be needed in production).
	
	auto max_tracker = make_shared<bonsai::global_max_tracker> ();
	dedisperser->add_processor(max_tracker);

	transform_chain.push_back(rf_pipelines::make_bonsai_dedisperser(dedisperser));

	// (transform_chain, outdir, json_output, verbosity)
	stream->run(transform_chain, string(), nullptr, 0);

	stringstream ss;
	ss << "bonsai: beam=" << ibeam 
	   << ", max_trigger=" << max_tracker->global_max_trigger
	   << ", dm=" << max_tracker->global_max_trigger_dm
	   << ", arrival_time=" << max_tracker->global_max_trigger_arrival_time << "\n";

	cout << ss.str().c_str() << flush;

    } catch (exception &e) {
	cerr << e.what() << "\n";
	throw;
    }
}


// -------------------------------------------------------------------------------------------------
//
// print_statistics()
//
// FIXME move equivalent functionality to ch_frb_io?


static void print_statistics(const l1_params &config, const vector<shared_ptr<ch_frb_io::intensity_network_stream>> &input_streams)
{
    assert((int)input_streams.size() == config.nstreams);

    for (int istream = 0; istream < config.nstreams; istream++) {
	cout << "stream " << istream << ": ipaddr=" << config.ipaddr[istream] << ", udp_port=" << config.port[istream] << endl;
 
	// vector<map<string,int>>
	auto statistics = input_streams[istream]->get_statistics();
	
	for (unsigned int irec = 0; irec < statistics.size(); irec++) {
	    cout << "    record " << irec  << endl;
	    const auto &s = statistics[irec];
	    
	    vector<string> keys;
	    for (const auto &kv: s)
		keys.push_back(kv.first);
	    
	    sort(keys.begin(), keys.end());
	    
	    for (const auto &k: keys) {
		auto kv = s.find(k);
		cout << "         " << k << " " << kv->second << endl;
	    }
	}
    }
}


// -------------------------------------------------------------------------------------------------


int main(int argc, char **argv)
{
    if (argc != 5)
	usage();

    // (l1_config_filename, rfi_config_filename, bonsai_config_filename, l1b_config_filename)
    l1_params config(argc, argv);

    int nstreams = config.nstreams;
    int nbeams = config.nbeams;

    vector<shared_ptr<ch_frb_io::intensity_network_stream>> input_streams(nstreams);
    vector<shared_ptr<L1RpcServer> > rpc_servers(nbeams);
    vector<std::thread> threads(nbeams);

    for (int istream = 0; istream < nstreams; istream++)
	input_streams[istream] = make_input_stream(config, istream);

    for (int istream = 0; istream < nstreams; istream++) {
	rpc_servers[istream] = make_l1rpc_server(config, istream, input_streams[istream]);
        // returns std::thread
        rpc_servers[istream]->start();
    }

    for (int ibeam = 0; ibeam < nbeams; ibeam++) {
	cerr << "spawning thread " << ibeam << endl;
	int nbeams_per_stream = xdiv(nbeams, nstreams);
	int istream = ibeam / nbeams_per_stream;
	threads[ibeam] = std::thread(dedispersion_thread_main, config, input_streams[istream], ibeam);
    }

    for (int ibeam = 0; ibeam < nbeams; ibeam++)
	threads[ibeam].join();

    if (config.verbosity >= 2)
	print_statistics(config, input_streams);

    return 0;
}
