#ifndef __CAR1_STRIPES_H__
#define __CAR1_STRIPES_H__

// The "SF Rush" STRIPE choice: Rush 1's own decals (src/car1_decals.cpp) as a ninth STRIPE value on the four cars that
// have one (src/car1_stripes.cpp, docs/rush1_research.md section 11). Needs the user's Rush 1 ROM.
namespace rush2::car1stripes {
    // The SF Rush Car Stripes option (Games tab, with the SF Rush Tracks option).
    void set_option(bool enabled);
}

#endif
