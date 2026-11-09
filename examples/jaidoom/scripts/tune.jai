// JaiDOOM tune.jai: presentation knobs the running game reloads. Edit a number and save while you play: the game
// reaches this file's import every frame, so the new value lands on the next frame and the J panel counts the
// reload. A mistake raises at the import and the game keeps the values it had.

float FOV = 90.0;             // the view's horizontal field of view, degrees (DOOM: 90)
float FIRE_REACH = 0.32;      // share of the screen height the title fire climbs
int FIRE_SHOW = 9;            // the coolest of the fire's 37 heats drawn over TITLEPIC (0 = all smoke, 36 = only white)
float BACKDROP_DIM = 0.35;    // brightness of the paused game behind the menu
