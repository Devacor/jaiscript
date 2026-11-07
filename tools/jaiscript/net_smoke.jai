// NetChannel loopback smoke — two channels in one process prove the runner's
// generic networking end to end: listen/connect handshake signals, framed
// binary-safe packets, reentrant echo from inside on_packet, and disconnect.
// Run: jaiscript net_smoke.jai   (exit 0 = green)

var FAILS = 0;
function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

var SRV_UP = 0;
var CLI_UP = 0;
var SRV_GOT = [];
var CLI_GOT = [];
var SRV_DOWN = "";

var srv = new NetChannel();
var cli = new NetChannel();

srv.on_connect.connect("up", []() { SRV_UP = SRV_UP + 1; });
cli.on_connect.connect("up", []() { CLI_UP = CLI_UP + 1; });
srv.on_packet.connect("recv", [](var bytes) {
	SRV_GOT.push(bytes);
	srv.send("echo:" + bytes);                       // reentrant send from inside pump
});
cli.on_packet.connect("recv", [](var bytes) { CLI_GOT.push(bytes); });
srv.on_disconnect.connect("down", [](var reason) { SRV_DOWN = reason; });

function pump_both(int times) -> void {
	for (int i = 0; i < times; ++i) {
		srv.pump();
		cli.pump();
		sleep(0.005);
	}
}

function main() -> int {
	check(srv.listen(28666), `listen (${srv.status()})`);
	check(cli.connect("127.0.0.1", 28666), `connect started (${cli.status()})`);
	int waited = 0;
	while ((SRV_UP == 0 || CLI_UP == 0) && waited < 200) { pump_both(1); waited = waited + 1; }
	check(SRV_UP == 1 && CLI_UP == 1, `both on_connect fired (srv ${SRV_UP} cli ${CLI_UP}, ${waited} pumps)`);
	check(srv.connected() && cli.connected(), "both report connected");

	// three packets, one pump cycle, one of them binary with NUL/0xFF
	var bin = "\x00\xFF" + to_char(1) + "mid\x00tail";
	cli.send("alpha");
	cli.send(bin);
	cli.send("gamma");
	pump_both(20);
	check(SRV_GOT.size() == 3, `server received 3 framed packets (${SRV_GOT.size()})`);
	check(SRV_GOT.size() == 3 && SRV_GOT[0] == "alpha" && SRV_GOT[2] == "gamma", "text packets intact + ordered");
	check(SRV_GOT.size() == 3 && SRV_GOT[1].size() == 11 && SRV_GOT[1] == bin, `binary packet byte-exact through framing (${SRV_GOT[1].size()} bytes)`);
	check(CLI_GOT.size() == 3 && CLI_GOT[1] == "echo:" + bin, "reentrant echo replies arrived binary-exact");

	// a large packet exercises partial sends/recvs across the 4KB pump chunks
	var big = "";
	for (int i = 0; i < 3000; ++i) { big += to_char(i & 255); }
	cli.send(big);
	pump_both(30);
	check(SRV_GOT.size() == 4 && SRV_GOT[3].size() == 3000 && SRV_GOT[3] == big, "3000-byte packet reassembled exactly");

	cli.close();
	pump_both(40);
	check(SRV_DOWN != "", `server saw the disconnect (${SRV_DOWN})`);
	check(!srv.connected(), `server no longer connected (${srv.status()})`);

	// connecting to a dead port reports failure via signal or status
	var lost = new NetChannel();
	var LOSTMSG = "";
	lost.on_disconnect.connect("down", [&](var reason) { LOSTMSG = reason; });
	lost.connect("127.0.0.1", 28667);
	// windows loopback refusal takes ~2.1s of SYN retries before the stack gives up
	for (int i = 0; i < 700 && LOSTMSG == "" && lost.status() == "connecting"; ++i) { lost.pump(); sleep(0.005); }
	check(LOSTMSG != "" || lost.status() != "connecting", `dead-port connect resolves (${lost.status()})`);

	net_server_section();

	print("");
	print(FAILS == 0 ? "NET SMOKE: ALL GREEN" : `NET SMOKE: ${FAILS} FAILURES`);
	return FAILS;
}

// ------------------------------------------------------------- NetServer ---
// N clients on one port: stable ids, targeted send, broadcast, kick, and the
// held-reason contract (a goodbye's frames deliver before its disconnect).

var SRV2_JOINS = [];
var SRV2_GOT = [];            // "id:bytes" arrival log
var SRV2_DROPS = [];          // "id:reason"
var PEER_A = [];
var PEER_B = [];
var PEER_C = [];
var PEER_D = [];
var PEER_NEST = [[], [], [], []];

function net_server_section() -> void {
	var host = new NetServer();
	host.on_client_connect.connect("j", [](var id) { SRV2_JOINS.push(id); });
	host.on_client_packet.connect("p", [](var id, var bytes) { SRV2_GOT.push(`${id}:${bytes}`); });
	host.on_client_disconnect.connect("d", [](var id, var reason) { SRV2_DROPS.push(`${id}:${reason}`); });
	check(host.listen(28766), "server listens");

	var peers = [];
	for (int p2 = 0; p2 < 4; ++p2) {
		var c = new NetChannel();
		c.connect("127.0.0.1", 28766);
		peers.push(c);
	}
	peers[0].on_packet.connect("r", [](var b) { PEER_A.push(b); });
	peers[1].on_packet.connect("r", [](var b) { PEER_B.push(b); });
	peers[2].on_packet.connect("r", [](var b) { PEER_C.push(b); });
	peers[3].on_packet.connect("r", [](var b) { PEER_D.push(b); });
	// pin: nested-element pushes land from inside a receiver (a second,
	// nested log on peer 1 must mirror PEER_B exactly)
	peers[1].on_packet.connect("nest", [](var b) { PEER_NEST[1].push(b); });

	for (int i = 0; i < 200 && host.client_count() < 4; ++i) {
		host.pump();
		for (int p3 = 0; p3 < 4; ++p3) { peers[p3].pump(); }
		sleep(0.005);
	}
	check(host.client_count() == 4, `4 clients connected (${host.client_count()})`);
	check(SRV2_JOINS.size() == 4 && SRV2_JOINS[0] == 1 && SRV2_JOINS[3] == 4, "ids assigned 1..4 in join order");

	// targeted send reaches exactly one client
	host.send(2, "only-two");
	for (int i2 = 0; i2 < 20; ++i2) {
		host.pump();
		for (int p4 = 0; p4 < 4; ++p4) { peers[p4].pump(); }
		sleep(0.005);
	}
	check(PEER_B.size() == 1 && PEER_B[0] == "only-two", "send(id) reached client 2");
	check(PEER_A.size() == 0 && PEER_C.size() == 0 && PEER_D.size() == 0, "send(id) reached ONLY client 2");

	// broadcast reaches all; per-client packets route by sender
	check(host.broadcast("all-hands") == 4, "broadcast reports 4 reached");
	peers[0].send("from-a");
	peers[2].send("from-c");
	for (int i3 = 0; i3 < 20; ++i3) {
		host.pump();
		for (int p5 = 0; p5 < 4; ++p5) { peers[p5].pump(); }
		sleep(0.005);
	}
	check(PEER_A.has("all-hands") && PEER_B.has("all-hands") && PEER_C.has("all-hands")
		&& PEER_D.has("all-hands"), "broadcast reached all 4");
	var nest1 = PEER_NEST[1];
	check(nest1.size() == PEER_B.size() && nest1.size() == 2 && nest1[0] == "only-two"
		&& nest1[1] == "all-hands", `nested-element pushes land from receivers (${nest1.size()})`);
	check(SRV2_GOT.has("1:from-a") && SRV2_GOT.has("3:from-c"), "server routes packets by sender id");

	// kick fires exactly client 2's disconnect; others keep working
	host.kick(2);
	for (int i4 = 0; i4 < 40; ++i4) {
		host.pump();
		for (int p7 = 0; p7 < 4; ++p7) { peers[p7].pump(); }
		sleep(0.005);
	}
	check(SRV2_DROPS.size() == 1 && SRV2_DROPS[0] == "2:kicked", `kick(2) dropped exactly client 2 (${SRV2_DROPS.size()})`);
	check(host.client_count() == 3, `count after kick (${host.client_count()})`);
	peers[3].send("still-alive");
	for (int i5 = 0; i5 < 20; ++i5) {
		host.pump();
		peers[3].pump();
		sleep(0.005);
	}
	check(SRV2_GOT.has("4:still-alive"), "survivors keep talking after a kick");

	// goodbye frames deliver before the disconnect (held-reason contract)
	peers[0].send("last-words");
	peers[0].close();
	for (int i6 = 0; i6 < 40; ++i6) {
		host.pump();
		sleep(0.005);
	}
	check(SRV2_GOT.has("1:last-words"), "frames sent with the close still delivered");
	check(SRV2_DROPS.has("1:closed by peer"), "then the disconnect fired");
	check(host.client_count() == 2, `count settles (${host.client_count()})`);
	host.close();
}

main()
