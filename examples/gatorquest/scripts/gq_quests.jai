// Gator Quest - quests and the journal. A quest is 0 (not taken), 1 (active)
// or 99 (done); quest_count tracks kills/catches/crafts toward its goal.

import "gq_magic.jai";

void quest_ensure(int slot) {
	Character& c = CHARS[slot];
	while (c.quest_stage.size() < QUESTS.size()) { c.quest_stage.push(0); }
	while (c.quest_count.size() < QUESTS.size()) { c.quest_count.push(0); }
}

bool quest_active(int slot, int q) { return CHARS[slot].quest_stage[q] == 1; }

int relics_held(int slot) {
	int n = 0;
	for (int i = 0; i < RELICS.size(); ++i) {
		if (pack_has(slot, RELICS[i], 1)) { n = n + 1; }
	}
	return n;
}

// progress toward the goal (for the journal and turn-in)
int quest_progress(int slot, int q) {
	QuestDef d = QUESTS[q];
	Character& c = CHARS[slot];
	if (q == Q_SIX_SCALES) { return relics_held(slot); }
	if (d.type == 1) { return pack_count(slot, d.target); }
	if (d.type == 3) {
		int n = 0;
		for (int i = 0; i < c.visited.size(); ++i) { n = n + c.visited[i]; }
		return n;
	}
	return c.quest_count[q];
}

int quest_goal(int q) { return q == Q_SIX_SCALES ? 6 : QUESTS[q].count; }

bool quest_ready(int slot, int q) { return quest_active(slot, q) && quest_progress(slot, q) >= quest_goal(q); }

void quest_bump(int slot, int q, int by) {
	Character& c = CHARS[slot];
	if (!quest_active(slot, q)) { return; }
	int before = c.quest_count[q];
	c.quest_count[q] = c.quest_count[q] + by;
	c.version = c.version + 1;
	if (before < QUESTS[q].count && c.quest_count[q] >= QUESTS[q].count) {
		say_to(slot, `Quest complete: ${QUESTS[q].name}. Return to ${ROLES[QUESTS[q].giver].name}.`, C_YELLOW);
		sound_at(MOB_WORLD[c.mob], MOB_X[c.mob], MOB_Y[c.mob], "quest");
	} else if (c.quest_count[q] < QUESTS[q].count) {
		say_to(slot, `${QUESTS[q].name}: ${c.quest_count[q]}/${QUESTS[q].count}`, C_GRAY);
	}
}

void quest_on_kill(int slot, int kind) {
	quest_ensure(slot);
	for (int q = 0; q < QUESTS.size(); ++q) {
		QuestDef d = QUESTS[q];
		if ((d.type == 0 || d.type == 2) && d.target == kind) { quest_bump(slot, q, 1); }
	}
}

void quest_on_visit(int slot) {
	quest_ensure(slot);
	for (int q = 0; q < QUESTS.size(); ++q) {
		if (QUESTS[q].type == 3 && quest_ready(slot, q)) {
			say_to(slot, `Quest complete: ${QUESTS[q].name}. Return to ${ROLES[QUESTS[q].giver].name}.`, C_YELLOW);
		}
	}
}

void quest_on_craft(int slot) {
	quest_ensure(slot);
	for (int q = 0; q < QUESTS.size(); ++q) {
		if (QUESTS[q].type == 4) { quest_bump(slot, q, 1); }
	}
}

void quest_on_fish(int slot) {
	quest_ensure(slot);
	for (int q = 0; q < QUESTS.size(); ++q) {
		if (QUESTS[q].type == 5) { quest_bump(slot, q, 1); }
	}
}

// quests an npc role can hand out right now (-1 = none)
int quest_offer(int slot, int role) {
	quest_ensure(slot);
	Character& c = CHARS[slot];
	for (int q = 0; q < QUESTS.size(); ++q) {
		QuestDef d = QUESTS[q];
		if (d.giver != role || c.quest_stage[q] != 0) { continue; }
		if (d.needs >= 0 && c.quest_stage[d.needs] != 99) { continue; }
		return q;
	}
	return -1;
}

// talking quest business with an npc: turn in anything finished, else take the next offer
void quest_talk(int slot, int role) {
	quest_ensure(slot);
	Character& c = CHARS[slot];
	for (int q = 0; q < QUESTS.size(); ++q) {
		QuestDef d = QUESTS[q];
		if (d.giver != role || !quest_ready(slot, q)) { continue; }
		if (d.type == 1) {
			int item = q == Q_SIX_SCALES ? -1 : d.target;
			if (item >= 0) { pack_take(slot, item, d.count); }
			else {
				for (int i = 0; i < RELICS.size(); ++i) { pack_take(slot, RELICS[i], 1); }
			}
		}
		c.quest_stage[q] = 99;
		c.gold = c.gold + d.gold;
		if (d.reward >= 0) {
			Item it = make_item(d.reward, d.reward_qty);
			if (!pack_add(slot, it)) { drop_near(it, MOB_WORLD[c.mob], MOB_X[c.mob], MOB_Y[c.mob], slot, 3000); }
		}
		c.fame = c.fame + 50 + d.gold / 2;
		c.version = c.version + 1;
		say_to(slot, `${ROLES[role].name}: "${d.done}"`, C_WHITE);
		string reward = d.reward >= 0 ? ` and ${item_name(make_item(d.reward, d.reward_qty))}` : "";
		say_to(slot, `Quest turned in: ${d.name}. You receive ${d.gold} gold${reward}.`, C_YELLOW);
		ev(Ev.banner, -1, 0, 0, -4, 0, 0, slot, "QUEST COMPLETE");
		sound_at(MOB_WORLD[c.mob], MOB_X[c.mob], MOB_Y[c.mob], "quest");
		fx_at(MOB_WORLD[c.mob], MOB_X[c.mob], MOB_Y[c.mob], Fx.gold, 16);
		if (q == Q_SIX_SCALES) {
			for (int s = 0; s < CHARS.size(); ++s) {
				if (CHARS[s].active) { say_to(s, `${c.name} has become the GRAND GATOR! All hail!`, C_YELLOW); }
			}
		}
		return;
	}
	int offer = quest_offer(slot, role);
	if (offer >= 0) {
		c.quest_stage[offer] = 1;
		c.quest_count[offer] = 0;
		c.version = c.version + 1;
		say_to(slot, `${ROLES[role].name}: "${QUESTS[offer].offer}"`, C_WHITE);
		say_to(slot, `New quest: ${QUESTS[offer].name} (see your journal, J).`, C_YELLOW);
		sound_at(MOB_WORLD[c.mob], MOB_X[c.mob], MOB_Y[c.mob], "chime");
		return;
	}
	for (int q = 0; q < QUESTS.size(); ++q) {
		if (QUESTS[q].giver == role && quest_active(slot, q)) {
			say_to(slot, `${ROLES[role].name}: "How goes ${QUESTS[q].name}? ${quest_progress(slot, q)}/${quest_goal(q)}."`, C_WHITE);
			return;
		}
	}
	array<string> lines = ROLES[role].lines.split(";");
	say_to(slot, `${ROLES[role].name}: "${lines[rnd(lines.size())]}"`, C_WHITE);
}

// journal lines for the HUD: active quests first, then finished ones
array<string> journal_lines(int slot) {
	quest_ensure(slot);
	array<string> out;
	Character& c = CHARS[slot];
	for (int q = 0; q < QUESTS.size(); ++q) {
		if (c.quest_stage[q] == 1) {
			string mark = quest_ready(slot, q) ? "[DONE] " : "";
			out.push(`${mark}${QUESTS[q].name} ${quest_progress(slot, q)}/${quest_goal(q)} - ${ROLES[QUESTS[q].giver].name}`);
		}
	}
	for (int q = 0; q < QUESTS.size(); ++q) {
		if (c.quest_stage[q] == 99) { out.push(`(done) ${QUESTS[q].name}`); }
	}
	return out;
}
