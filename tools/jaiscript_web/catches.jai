// The wasm gate's check that the library catches the exceptions it throws, that the browser's entropy seeds a generator,
// and that a program which sleeps outlasts the one-second budget the gate runs it under
string caught = "uncaught";
try {
    string missing = read_file("no such file");
} catch (e) {
    caught = "caught";
}
int turns = 0;
for (int nap = 0; nap < 30; ++nap) {
    for (int i = 0; i < 4000; ++i) { turns += 1; }   // the budget reads its clock every 1024 loop turns
    sleep(0.05);
}
random dice;
int roll = dice.next_int(1, 6);
print("x".to_int(-1), " ", "42".to_int(), " ", caught, " ", roll >= 1 && roll <= 6);
