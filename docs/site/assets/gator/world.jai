// The stage gator.jai plays on: the cake and the Mob base class. The landing page runs it once, before gator.jai.
class Cake {
    int x = 530;
    int left = 6;
    int edge() { return x + (6 - math::max(left, 1)) * 10 - 50; }   // the gator stops at the first slice left
}
Cake cake;

class Mob {
    int x = 30;
    int chewing = 0;
    string words = "";
    void step_toward(Cake& target, int speed) { x = math::clamp(x + speed, 30, target.edge()); }
    bool beside(Cake& target) { return x >= target.edge(); }
    void say(string text) {
        words = text;
        chewing = 50;
    }
}
