// Plays a program that declares bool frame(): calls it until it answers false, and between frames applies whatever was
// saved to the program's file. The sandbox runs this for such a program, saving the editor to program.jai as you type.
// In a terminal: jaiscript live.jai -- cakewalk.jai [frames to play]
extern function frame;
string PROGRAM = ARGS.size() > 0 ? ARGS[0] : "program.jai";
int FRAMES_TO_PLAY = ARGS.size() > 1 ? ARGS[1].to_int() : 0;
string NOTICE_ROW = `\x1b[${term_rows()};1H\x1b[0m`;

// A rejected edit's listing names the mistake on its first line that says "error"
string reasonOf(string a_listing) {
	array<string> lines = a_listing.split("\n");
	return lines.find(line => line.contains("error")) ?? lines[0];
}

import(PROGRAM);
for (int played = 1; frame() && played != FRAMES_TO_PLAY; ++played) {
	try {
		if (import(PROGRAM)) {
			print(NOTICE_ROW + "\x1b[K", skip_newline);
		}
	} catch (rejected) {
		print(NOTICE_ROW + "\x1b[41m " + reasonOf(to_string(rejected)) + " \x1b[0m\x1b[K", skip_newline);
	}
}
