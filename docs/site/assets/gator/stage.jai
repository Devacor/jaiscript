// The landing page's frame loop, run after gator.jai: the page calls frame() sixty times a second and draws what state() says
string state() { return `${gator.x} ${gator.spd} ${gator.slices} ${gator.chewing} ${gator.words}`; }

string frame() {
    cake.left = gator.slices;
    if (gator.chewing > 0) {
        if (--gator.chewing == 0) {
            gator.x = 30;
            if (gator.slices <= 0) { gator.slices = Gator().slices; }
        }
    } else {
        gator.tick();
    }
    return state();
}
