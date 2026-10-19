#ifndef slic3r_BrickLayers_hpp_
#define slic3r_BrickLayers_hpp_

namespace Slic3r {

class PrintObject;

// Brick layers, also called staggered perimeters: every other inner wall
// (inset 1, 3, 5, ...) is printed half a layer higher than the walls next to
// it, so the walls interlock along Z the way bricks are laid. The outer wall
// stays at the layer height.
//
// The Z and flow rules follow the Brick Layers post-processing scripts by
// Roman Tenger (TenTech, GPL-3.0) and Everson Siqueira (GeekDetour, GPL-3.0):
// a lifted wall sits at print_z + h/2 with normal flow, the first one of a
// stack gets 1.5x flow to reach down to the layer below, and the last one is
// printed at print_z with 0.5x flow so it ends flush.
//
// Here the start and end of a stack are found per piece of wall by comparing
// it with the lifted walls of the layers below and above, so steps, holes
// and tops in the middle of a part are handled too, and walls that drift
// sideways on a slope fall back to normal walls.
//
// Must run after the walls of all layers exist. Lifted pieces get
// ExtrusionPath::z_offset > 0; the G-code generator prints them at
// print_z + z_offset after the rest of their island.
void apply_brick_layers(PrintObject &object);

} // namespace Slic3r

#endif // slic3r_BrickLayers_hpp_
