// Extruder stepper pulse time generation
//
// Copyright (C) 2018-2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stddef.h> // offsetof
#include <stdlib.h> // malloc
#include <string.h> // memset
#include "compiler.h" // __visible
#include "itersolve.h" // struct stepper_kinematics
#include "pyhelper.h" // errorf
#include "trapq.h" // move_get_distance

// Without pressure advance, the extruder stepper position is:
//     extruder_position(t) = nominal_position(t)
// When pressure advance is enabled, additional filament is pushed
// into the extruder during acceleration (and retracted during
// deceleration). The formula for additional filament length is:
//     pa(t) = pressure_advance * nominal_velocity(t)
// Which is then "smoothed" using a weighted average:
//     smooth_position(t) = nominal_position(t) + (
//         definitive_integral(pa(x) * (smooth_time/2 - abs(t-x)) * dx,
//                             from=t-smooth_time/2, to=t+smooth_time/2)
//         / ((smooth_time/2)**2))

// Calculate the definitive integral of the motion formula:
//   pa(t) = delta_base + t * start_delta_v
static double
pa_integrate(double delta_base, double start_dv, double start, double end)
{
    double half_dv = .5 * start_dv;
    double si = start * (delta_base + start * half_dv);
    double ei = end * (delta_base + end * half_dv);
    return ei - si;
}

// Calculate the definitive integral of time weighted position:
//   weighted_pa(t) = t * (delta_base + t * delta_start_v)
static double
pa_integrate_time(double delta_base, double start_dv, double start, double end)
{
    double half_db = .5 * delta_base, third_deltav = (1. / 3.) * start_dv;
    double si  = start * start * (half_db + start * third_deltav);
    double ei  = end * end * (half_db + end * third_deltav);
    return ei - si;
}

// Calculate the definitive integral of extruder for a given move
static double
pa_move_integrate(struct move *m, double pressure_advance
                 , double start, double end, double time_offset)
{
    if (start < 0.)
        start = 0.;
    if (end > m->move_t)
        end = m->move_t;
    // Calculate base position and velocity with pressure advance
    int can_pressure_advance = m->axes_r.y != 0.;
    if (!can_pressure_advance)
        pressure_advance = 0.;
    double delta_base = pressure_advance * m->start_v;
    double start_dv = pressure_advance * 2. * m->half_accel;
    // Calculate definitive integral
    double iext = pa_integrate(delta_base, start_dv, start, end);
    double wgt_ext = pa_integrate_time(delta_base, start_dv, start, end);
    return wgt_ext - time_offset * iext;
}

// Calculate the definitive integral of the extruder over a range of moves
static double
pa_range_integrate(struct move *m, double move_time
                   , double pressure_advance, double hst)
{
    // Calculate integral for the current move
    double res = 0., start = move_time - hst, end = move_time + hst;
    res += pa_move_integrate(m, pressure_advance, start, move_time, start);
    res -= pa_move_integrate(m, pressure_advance, move_time, end, end);
    // Integrate over previous moves
    struct move *prev = m;
    while (unlikely(start < 0.)) {
        prev = list_prev_entry(prev, node);
        start += prev->move_t;
        res += pa_move_integrate(prev, pressure_advance, start
                                 , prev->move_t, start);
    }
    // Integrate over future moves
    while (unlikely(end > m->move_t)) {
        end -= m->move_t;
        m = list_next_entry(m, node);
        res -= pa_move_integrate(m, pressure_advance, 0., end, end);
    }
    return res;
}

struct extruder_stepper {
    struct stepper_kinematics sk;
    double pressure_advance, half_smooth_time, inv_half_smooth_time2;
};

static double
extruder_calc_position(struct stepper_kinematics *sk, struct move *m
                       , double move_time)
{
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    double base = m->start_pos.x + move_get_distance(m, move_time);
    double hst = es->half_smooth_time;
    if (!hst)
        // Pressure advance not enabled
        return base;
    // Apply pressure advance and average over smooth_time
    double area = pa_range_integrate(m, move_time, es->pressure_advance, hst);
    return base + area * es->inv_half_smooth_time2;
}

void __visible
extruder_set_pressure_advance(struct stepper_kinematics *sk
                              , double pressure_advance, double smooth_time)
{
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    double hst = smooth_time * .5;
    es->half_smooth_time = hst;
    es->sk.gen_steps_pre_active = es->sk.gen_steps_post_active = hst;
    if (! hst)
        return;
    es->inv_half_smooth_time2 = 1. / (hst * hst);
    es->pressure_advance = pressure_advance;
}

struct stepper_kinematics * __visible
extruder_stepper_alloc(void)
{
    struct extruder_stepper *es = malloc(sizeof(*es));
    memset(es, 0, sizeof(*es));
    es->sk.calc_position_cb = extruder_calc_position;
    es->sk.active_flags = AF_X;
    return &es->sk;
}
