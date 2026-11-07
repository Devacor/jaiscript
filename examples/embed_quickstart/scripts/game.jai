// Entry script. The host runs it with execute_file, then calls tick() once per frame.
// Vec2, Actor, Monster, spawn_offset() and host_log() come from the host.

import "rules.jai";

Tuning tuning;
int frames_run;
array<string> journal;
map<string, int> kills = {"orc": 3, "troll": 1};
array<var> loot = [12, "rope", 2.5];

void explode(string why) { throw why; }

int tick(int frame) {
	import("rules.jai");            // reached every frame: a touched rules.jai re-runs here
	frames_run += 1;
	int dmg = damage_for(frame);
	if (frame % 5 == 0) { dmg += tuning.crit; }
	return dmg;
}

function spawn(string name, int hp) {
	auto m = Monster(name, hp);
	m.pos = Vec2(1.0, 2.0) + spawn_offset();
	m.on_hurt.connect("journal", [=](string who, int left) -> void {
		journal.push(`${who} has ${left} hp`);
	});
	host_log(`spawned ${m.describe()} at (${m.pos.x}, ${m.pos.y})`);
	return m;
}

int total(array<int> values) {
	int sum = 0;
	for (auto v : values) { sum += v; }
	return sum;
}

int average(array<int> values) {
	return total(values) / values.size();
}

string snapshot() {
	return to_json({"frames": frames_run, "hits": hits_dealt, "journal": journal});
}

void restore(string text) {
	var state = from_json(text);
	frames_run = state["frames"];
	journal = state["journal"];
}
