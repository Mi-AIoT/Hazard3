#include <iostream>
#include <fstream>
#include <cstdint>
#include <string>
#include <stdio.h>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Device-under-test model generated by CXXRTL:
#include "dut.cpp"
#include <backends/cxxrtl/cxxrtl_vcd.h>

// There must be a better way
#ifdef __x86_64__
#define I64_FMT "%ld"
#else
#define I64_FMT "%lld"
#endif

// -----------------------------------------------------------------------------

static const int MEM_SIZE = 16 * 1024 * 1024;
static const int N_RESERVATIONS = 2;
static const uint32_t RESERVATION_ADDR_MASK = 0xfffffff8u;

static const unsigned int IO_BASE = 0x80000000;
enum {
	IO_PRINT_CHAR  = 0x000,
	IO_PRINT_U32   = 0x004,
	IO_EXIT        = 0x008,
	IO_SET_SOFTIRQ = 0x010,
	IO_CLR_SOFTIRQ = 0x014,
	IO_GLOBMON_EN  = 0x018,
	IO_SET_IRQ     = 0x020,
	IO_CLR_IRQ     = 0x030,
	IO_MTIME       = 0x100,
	IO_MTIMEH      = 0x104,
	IO_MTIMECMP    = 0x108,
	IO_MTIMECMPH   = 0x10c
};

struct mem_io_state {
	uint64_t mtime;
	uint64_t mtimecmp;

	bool exit_req;
	uint32_t exit_code;

	uint8_t *mem;

	bool monitor_enabled;
	bool reservation_valid[2];
	uint64_t reservation_addr[2];

	mem_io_state() {
		mtime = 0;
		mtimecmp = 0;
		exit_req = false;
		exit_code = 0;
		monitor_enabled = false;
		for (int i = 0; i < N_RESERVATIONS; ++i) {
			reservation_valid[i] = false;
			reservation_addr[i] = 0;
		}
		mem = new uint8_t[MEM_SIZE];
		for (size_t i = 0; i < MEM_SIZE; ++i)
			mem[i] = 0;
	}

	// Where we're going we don't need a destructor B-)

	void step(cxxrtl_design::p_tb &tb) {
		// Default update logic for mtime, mtimecmp
		++mtime;
		tb.p_timer__irq.set<bool>(mtime >= mtimecmp);
	}
};

typedef enum {
	SIZE_BYTE = 0,
	SIZE_HWORD = 1,
	SIZE_WORD = 2,
	SIZE_DWORD = 2
} bus_size_t;

struct bus_request {
	uint64_t addr;
	bus_size_t size;
	bool write;
	bool excl;
	uint64_t wdata;
	int reservation_id;
	bus_request(): addr(0), size(SIZE_BYTE), write(0), excl(0), wdata(0), reservation_id(0) {}
};

struct bus_response {
	uint64_t rdata;
	int stall_cycles;
	bool err;
	bool exokay;
	bus_response(): rdata(0), stall_cycles(0), err(false), exokay(true) {}
};

bus_response mem_access(cxxrtl_design::p_tb &tb, mem_io_state &memio, bus_request req) {
	bus_response resp;

	// Global monitor. When monitor is not enabled, HEXOKAY is tied high
	if (memio.monitor_enabled) {
		if (req.excl) {
			// Always set reservation on read. Always clear reservation on
			// write. On successful write, clear others' matching reservations.
			if (req.write) {
				resp.exokay = memio.reservation_valid[req.reservation_id] &&
					memio.reservation_addr[req.reservation_id] == (req.addr & RESERVATION_ADDR_MASK);
				memio.reservation_valid[req.reservation_id] = false;
				if (resp.exokay) {
					for (int i = 0; i < N_RESERVATIONS; ++i) {
						if (i == req.reservation_id)
							continue;
						if (memio.reservation_addr[i] == (req.addr & RESERVATION_ADDR_MASK))
							memio.reservation_valid[i] = false;
					}
				}
			}
			else {
				resp.exokay = true;
				memio.reservation_valid[req.reservation_id] = true;
				memio.reservation_addr[req.reservation_id] = req.addr & RESERVATION_ADDR_MASK;
			}
		}
		else {
			resp.exokay = false;
			// Non-exclusive write still clears others' reservations
			if (req.write) {
				for (int i = 0; i < N_RESERVATIONS; ++i) {
					if (i == req.reservation_id)
						continue;
					if (memio.reservation_addr[i] == (req.addr & RESERVATION_ADDR_MASK))
						memio.reservation_valid[i] = false;
				}
			}
		}
	}


	if (req.write) {
		if (memio.monitor_enabled && req.excl && !resp.exokay) {
			// Failed exclusive write; do nothing
		}
		else if (req.addr <= MEM_SIZE - 8u) {
			unsigned int n_bytes = 1u << (int)req.size;
			// Note we are relying on hazard3's byte lane replication
			for (unsigned int i = 0; i < n_bytes; ++i) {
				memio.mem[req.addr + i] = req.wdata >> (8 * i) & 0xffu;
			}
		}
		else if (req.addr == IO_BASE + IO_PRINT_CHAR) {
			putchar(req.wdata);
		}
		else if (req.addr == IO_BASE + IO_PRINT_U32) {
			printf("%08x\n", (uint32_t)req.wdata);
		}
		else if (req.addr == IO_BASE + IO_EXIT) {
			if (!memio.exit_req) {
				memio.exit_req = true;
				memio.exit_code = req.wdata;
			}
		}
		else if (req.addr == IO_BASE + IO_SET_SOFTIRQ) {
			tb.p_soft__irq.set<uint8_t>(tb.p_soft__irq.get<uint8_t>() | req.wdata);
		}
		else if (req.addr == IO_BASE + IO_CLR_SOFTIRQ) {
			tb.p_soft__irq.set<uint8_t>(tb.p_soft__irq.get<uint8_t>() & ~req.wdata);
		}
		else if (req.addr == IO_BASE + IO_GLOBMON_EN) {
			memio.monitor_enabled = req.wdata;
		}
		else if (req.addr == IO_BASE + IO_SET_IRQ) {
			tb.p_irq.set<uint32_t>(tb.p_irq.get<uint32_t>() | req.wdata);
		}
		else if (req.addr == IO_BASE + IO_CLR_IRQ) {
			tb.p_irq.set<uint32_t>(tb.p_irq.get<uint32_t>() & ~req.wdata);
		}
		else if (req.addr == IO_BASE + IO_MTIME) {
			// TODO 64-bit IO
			memio.mtime = (memio.mtime & 0xffffffff00000000u) | req.wdata;
		}
		else if (req.addr == IO_BASE + IO_MTIMEH) {
			memio.mtime = (memio.mtime & 0x00000000ffffffffu) | ((uint64_t)req.wdata << 32);
		}
		else if (req.addr == IO_BASE + IO_MTIMECMP) {
			memio.mtimecmp = (memio.mtimecmp & 0xffffffff00000000u) | req.wdata;
		}
		else if (req.addr == IO_BASE + IO_MTIMECMPH) {
			memio.mtimecmp = (memio.mtimecmp & 0x00000000ffffffffu) | ((uint64_t)req.wdata << 32);
		}
		else {
			resp.err = true;
		}
	}
	else {
		if (req.addr <= MEM_SIZE - (1u << (int)req.size)) {
			req.addr &= ~0x7u;
			resp.rdata =
				(uint64_t)memio.mem[req.addr] |
				(uint64_t)memio.mem[req.addr + 1] << 8 |
				(uint64_t)memio.mem[req.addr + 2] << 16 |
				(uint64_t)memio.mem[req.addr + 3] << 24 |
				(uint64_t)memio.mem[req.addr + 4] << 32 |
				(uint64_t)memio.mem[req.addr + 5] << 40 |
				(uint64_t)memio.mem[req.addr + 6] << 48 |
				(uint64_t)memio.mem[req.addr + 7] << 56;
		}
		else if (req.addr == IO_BASE + IO_SET_SOFTIRQ || req.addr == IO_BASE + IO_CLR_SOFTIRQ) {
			// TODO 64-bit IO
			resp.rdata = tb.p_soft__irq.get<uint8_t>();
		}
		else if (req.addr == IO_BASE + IO_SET_IRQ || req.addr == IO_BASE + IO_CLR_IRQ) {
			resp.rdata = tb.p_irq.get<uint32_t>();
		}
		else if (req.addr == IO_BASE + IO_MTIME) {
			resp.rdata = memio.mtime;
		}
		else if (req.addr == IO_BASE + IO_MTIMEH) {
			resp.rdata = memio.mtime >> 32;
		}
		else if (req.addr == IO_BASE + IO_MTIMECMP) {
			resp.rdata = memio.mtimecmp;
		}
		else if (req.addr == IO_BASE + IO_MTIMECMPH) {
			resp.rdata = memio.mtimecmp >> 32;
		}
		else {
			resp.err = true;
		}
	}
	if (resp.err) {
		resp.exokay = false;
	}
	return resp;
}

// -----------------------------------------------------------------------------

const char *help_str =
"Usage: tb [--bin x.bin] [--port n] [--vcd x.vcd] [--dump start end] \\\n"
"          [--cycles n] [--cpuret]\n"
"\n"
"    --bin x.bin      : Flat binary file loaded to address 0x0 in RAM\n"
"    --vcd x.vcd      : Path to dump waveforms to\n"
"    --dump start end : Print out memory contents from start to end (exclusive)\n"
"                       after execution finishes. Can be passed multiple times.\n"
"    --cycles n       : Maximum number of cycles to run before exiting.\n"
"                       Default is 0 (no maximum).\n"
"    --port n         : Port number to listen for openocd remote bitbang. Sim\n"
"                       runs in lockstep with JTAG bitbang, not free-running.\n"
"    --cpuret         : Testbench's return code is the return code written to\n"
"                       IO_EXIT by the CPU, or -1 if timed out.\n"
;

void exit_help(std::string errtext = "") {
	std::cerr << errtext << help_str;
	exit(-1);
}

static const int TCP_BUF_SIZE = 256;

int main(int argc, char **argv) {

	bool load_bin = false;
	std::string bin_path;
	bool dump_waves = false;
	std::string waves_path;
	std::vector<std::pair<uint32_t, uint32_t>> dump_ranges;
	int64_t max_cycles = 0;
	bool propagate_return_code = false;
	uint16_t port = 0;

	for (int i = 1; i < argc; ++i) {
		std::string s(argv[i]);
		if (s.rfind("--", 0) != 0) {
			std::cerr << "Unexpected positional argument " << s << "\n";
			exit_help("");
		}
		else if (s == "--bin") {
			if (argc - i < 2)
				exit_help("Option --bin requires an argument\n");
			load_bin = true;
			bin_path = argv[i + 1];
			i += 1;
		}
		else if (s == "--vcd") {
			if (argc - i < 2)
				exit_help("Option --vcd requires an argument\n");
			dump_waves = true;
			waves_path = argv[i + 1];
			i += 1;
		}
		else if (s == "--dump") {
			if (argc - i < 3)
				exit_help("Option --dump requires 2 arguments\n");
			dump_ranges.push_back(std::pair<uint32_t, uint32_t>(
				std::stoul(argv[i + 1], 0, 0),
				std::stoul(argv[i + 2], 0, 0)
			));;
			i += 2;
		}
		else if (s == "--cycles") {
			if (argc - i < 2)
				exit_help("Option --cycles requires an argument\n");
			max_cycles = std::stol(argv[i + 1], 0, 0);
			i += 1;
		}
		else if (s == "--port") {
			if (argc - i < 2)
				exit_help("Option --port requires an argument\n");
			port = std::stol(argv[i + 1], 0, 0);
			i += 1;
		}
		else if (s == "--cpuret") {
			propagate_return_code = true;
		}
		else {
			std::cerr << "Unrecognised argument " << s << "\n";
			exit_help("");
		}
	}
	if (!(load_bin || port != 0))
		exit_help("At least one of --bin or --port must be specified.\n");

	int server_fd, sock_fd;
	struct sockaddr_in sock_addr;
	int sock_opt = 1;
	socklen_t sock_addr_len = sizeof(sock_addr);
	char txbuf[TCP_BUF_SIZE], rxbuf[TCP_BUF_SIZE];
	int rx_ptr = 0, rx_remaining = 0, tx_ptr = 0;

	if (port != 0) {
		server_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (server_fd == 0) {
			fprintf(stderr, "socket creation failed\n");
			exit(-1);
		}

		int setsockopt_rc = setsockopt(
			server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
			&sock_opt, sizeof(sock_opt)
		);

		if (setsockopt_rc) {
			fprintf(stderr, "setsockopt failed\n");
			exit(-1);
		}

		sock_addr.sin_family = AF_INET;
		sock_addr.sin_addr.s_addr = INADDR_ANY;
		sock_addr.sin_port = htons(port);
		if (bind(server_fd, (struct sockaddr *)&sock_addr, sizeof(sock_addr)) < 0) {
			fprintf(stderr, "bind failed\n");
			exit(-1);
		}

		printf("Waiting for connection on port %u\n", port);
		if (listen(server_fd, 3) < 0) {
			fprintf(stderr, "listen failed\n");
			exit(-1);
		}
		sock_fd = accept(server_fd, (struct sockaddr *)&sock_addr, &sock_addr_len);
		if (sock_fd < 0) {
			fprintf(stderr, "accept failed\n");
			exit(-1);
		}
		printf("Connected\n");
	}

	mem_io_state memio;

	if (load_bin) {
		std::ifstream fd(bin_path, std::ios::binary | std::ios::ate);
		if (!fd){
			std::cerr << "Failed to open \"" << bin_path << "\"\n";
			return -1;
		}
		std::streamsize bin_size = fd.tellg();
		if (bin_size > MEM_SIZE) {
			std::cerr << "Binary file (" << bin_size << " bytes) is larger than memory (" << MEM_SIZE << " bytes)\n";
			return -1;
		}
		fd.seekg(0, std::ios::beg);
		fd.read((char*)memio.mem, bin_size);
	}

	cxxrtl_design::p_tb top;

	std::ofstream waves_fd;
	cxxrtl::vcd_writer vcd;
	if (dump_waves) {
		waves_fd.open(waves_path);
		cxxrtl::debug_items all_debug_items;
		top.debug_info(all_debug_items);
		vcd.timescale(1, "us");
		vcd.add(all_debug_items);
	}

	// Loop-carried address-phase requests
	bus_request req_i;
	bus_request req_d;
	bool req_i_vld = false;
	bool req_d_vld = false;
	req_i.reservation_id = 0;
	req_d.reservation_id = 1;

	// Set bus interfaces to generate good IDLE responses at first
	top.p_i__hready.set<bool>(true);
	top.p_d__hready.set<bool>(true);

	// Reset + initial clock pulse

	top.step();
	top.p_clk.set<bool>(true);
	top.p_tck.set<bool>(true);
	top.step();
	top.p_clk.set<bool>(false);
	top.p_tck.set<bool>(false);
	top.p_trst__n.set<bool>(true);
	top.p_rst__n.set<bool>(true);
	top.step();
	top.step(); // workaround for github.com/YosysHQ/yosys/issues/2780

	bool timed_out = false;
	for (int64_t cycle = 0; cycle < max_cycles || max_cycles == 0; ++cycle) {
		top.p_clk.set<bool>(false);
		top.step();
		if (dump_waves)
			vcd.sample(cycle * 2);
		top.p_clk.set<bool>(true);
		top.step();
		top.step(); // workaround for github.com/YosysHQ/yosys/issues/2780

		// If --port is specified, we run the simulator in lockstep with the
		// remote bitbang commands, to get more consistent simulation traces.
		// This slows down simulation quite a bit compared with normal
		// free-running.
		//
		// Most bitbang commands complete in one cycle (e.g. TCK/TMS/TDI
		// writes) but reads take 0 cycles, step=false.
		bool got_exit_cmd = false;
		bool step = false;
		if (port != 0) {
			while (!step) {
				if (rx_remaining > 0) {
					char c = rxbuf[rx_ptr++];
					--rx_remaining;

					if (c == 'r' || c == 's') {
						top.p_trst__n.set<bool>(true);
						step = true;
					}
					else if (c == 't' || c == 'u') {
						top.p_trst__n.set<bool>(false);
					}
					else if (c >= '0' && c <= '7') {
						int mask = c - '0';
						top.p_tck.set<bool>(mask & 0x4);
						top.p_tms.set<bool>(mask & 0x2);
						top.p_tdi.set<bool>(mask & 0x1);
						step = true;
					}
					else if (c == 'R') {
						txbuf[tx_ptr++] = top.p_tdo.get<bool>() ? '1' : '0';
						if (tx_ptr >= TCP_BUF_SIZE || rx_remaining == 0) {
							send(sock_fd, txbuf, tx_ptr, 0);
							tx_ptr = 0;
						}
					}
					else if (c == 'Q') {
						printf("OpenOCD sent quit command\n");
						got_exit_cmd = true;
						step = true;
					}
				}
				else {
					// Potentially the last command was not a read command, but
					// OpenOCD is still waiting for a last response from its
					// last command packet before it sends us any more, so now is
					// the time to flush TX.
					if (tx_ptr > 0) {
						send(sock_fd, txbuf, tx_ptr, 0);
						tx_ptr = 0;
					}	
					rx_ptr = 0;
					rx_remaining = read(sock_fd, &rxbuf, TCP_BUF_SIZE);
				}
			}
		}

		memio.step(top);

		// The two bus ports are handled identically. This enables swapping out of
		// various `tb.v` hardware integration files containing:
		//
		// - A single, dual-ported processor (instruction fetch, load/store ports)
		// - A single, single-ported processor (instruction fetch + load/store muxed internally)
		// - A pair of single-ported processors, for dual-core debug tests

		if (top.p_d__hready.get<bool>()) {
			// Clear bus error by default
			top.p_d__hresp.set<bool>(false);

			// Handle current data phase
			req_d.wdata = top.p_d__hwdata.get<uint64_t>();
			bus_response resp;
			if (req_d_vld)
				resp = mem_access(top, memio, req_d);
			else
				resp.exokay = !memio.monitor_enabled;
			if (resp.err) {
				// Phase 1 of error response
				top.p_d__hready.set<bool>(false);
				top.p_d__hresp.set<bool>(true);
			}
			top.p_d__hrdata.set<uint64_t>(resp.rdata);
			top.p_d__hexokay.set<bool>(resp.exokay);

			// Progress current address phase to data phase
			req_d_vld = top.p_d__htrans.get<uint8_t>() >> 1;
			req_d.write = top.p_d__hwrite.get<bool>();
			req_d.size = (bus_size_t)top.p_d__hsize.get<uint8_t>();
			req_d.addr = top.p_d__haddr.get<uint64_t>();
			req_d.excl = top.p_d__hexcl.get<bool>();
		}
		else {
			// hready=0. Currently this only happens when we're in the first
			// phase of an error response, so go to phase 2.
			top.p_d__hready.set<bool>(true);
		}


		if (top.p_i__hready.get<bool>()) {
			top.p_i__hresp.set<bool>(false);

			req_i.wdata = top.p_i__hwdata.get<uint64_t>();
			bus_response resp;
			if (req_i_vld)
				resp = mem_access(top, memio, req_i);
			else
				resp.exokay = !memio.monitor_enabled;
			if (resp.err) {
				// Phase 1 of error response
				top.p_i__hready.set<bool>(false);
				top.p_i__hresp.set<bool>(true);
			}
			top.p_i__hrdata.set<uint64_t>((resp.rdata >> (req_i.addr & 0x4 ? 32 : 0)) & 0xffffffffu);
			top.p_i__hexokay.set<bool>(resp.exokay);

			// Progress current address phase to data phase
			req_i_vld = top.p_i__htrans.get<uint8_t>() >> 1;
			req_i.write = top.p_i__hwrite.get<bool>();
			req_i.size = (bus_size_t)top.p_i__hsize.get<uint8_t>();
			req_i.addr = top.p_i__haddr.get<uint64_t>();
			req_i.excl = top.p_i__hexcl.get<bool>();
		}
		else {
			// hready=0. Currently this only happens when we're in the first
			// phase of an error response, so go to phase 2.
			top.p_i__hready.set<bool>(true);
		}

		if (dump_waves) {
			// The extra step() is just here to get the bus responses to line up nicely
			// in the VCD (hopefully is a quick update)
			top.step();
			vcd.sample(cycle * 2 + 1);
			waves_fd << vcd.buffer;
			vcd.buffer.clear();
		}

		if (memio.exit_req) {
			printf("CPU requested halt. Exit code %d\n", memio.exit_code);
			printf("Ran for " I64_FMT " cycles\n", cycle + 1);
			break;
		}
		if (cycle + 1 == max_cycles) {
			printf("Max cycles reached\n");
			timed_out = true;
		}
		if (got_exit_cmd)
			break;
	}

	close(sock_fd);

	for (auto r : dump_ranges) {
		printf("Dumping memory from %08x to %08x:\n", r.first, r.second);
		for (int i = 0; i < r.second - r.first; ++i)
			printf("%02x%c", memio.mem[r.first + i], i % 16 == 15 ? '\n' : ' ');
		printf("\n");
	}

	if (propagate_return_code && timed_out) {
		return -1;
	}
	else if (propagate_return_code && memio.exit_req) {
		return memio.exit_code;
	}
	else {
		return 0;
	}
}
