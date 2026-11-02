/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* OPTIONAL LOCAL POLICY, not Linux hardware configuration or Apple's
 * calibrated CoreBrightness algorithm. No IO; decisions must be committed
 * only after the backlight service accepts the request. Default disabled. */
#ifndef COACHZ_AUTO_BRIGHTNESS_H
#define COACHZ_AUTO_BRIGHTNESS_H
#include <stdint.h>

struct cab_point {uint32_t lux, millinits;};
/* Nominal 20..400 nits from current CoachzBacklight; replace/calibrate later.
 * This example preference curve is not a panel or sensor calibration. */
static const struct cab_point cab_curve[] = {
    {0,20000},{5,35000},{20,60000},{100,110000},
    {400,180000},{1000,260000},{5000,400000},{65535,400000}
};
enum {CAB_MIN=20000, CAB_MAX=400000, CAB_MAX_GAP_MS=2000, CAB_MANUAL_MS=30000};
struct cab_policy {
    uint8_t enabled, awake, have_sample, have_candidate, have_request;
    uint32_t filtered_millilux, candidate, requested;
    uint64_t last_ms, candidate_ms, hold_until_ms;
};
struct cab_action {uint8_t apply; uint32_t millinits, fade_ms;};
static inline uint32_t cab_clamp(uint32_t n) {return n<CAB_MIN?CAB_MIN:n>CAB_MAX?CAB_MAX:n;}
static inline uint32_t cab_distance(uint32_t a,uint32_t b) {return a>b?a-b:b-a;}
static inline uint32_t cab_target(uint32_t millilux) {
    for (unsigned i=1;i<sizeof(cab_curve)/sizeof(cab_curve[0]);++i) {
        uint32_t hi=cab_curve[i].lux*1000u,lo=cab_curve[i-1].lux*1000u;
        if (millilux<=hi) return cab_curve[i-1].millinits+
            (uint32_t)((uint64_t)(millilux-lo)*(cab_curve[i].millinits-cab_curve[i-1].millinits)/(hi-lo));
    }
    return CAB_MAX;
}
static inline void cab_reset_tracking(struct cab_policy *p) {
    p->have_sample=0; p->have_candidate=0;
}
static inline void cab_enable(struct cab_policy *p,int enabled,int awake) {
    p->enabled=enabled!=0; p->awake=awake!=0;
    p->have_request=0; p->hold_until_ms=0;
    cab_reset_tracking(p);
}
static inline void cab_manual(struct cab_policy *p,uint64_t now_ms) {
    p->hold_until_ms=now_ms>UINT64_MAX-CAB_MANUAL_MS?UINT64_MAX:now_ms+CAB_MANUAL_MS;
    p->have_request=0; cab_reset_tracking(p);
}
static inline struct cab_action cab_update(struct cab_policy *p,uint64_t now_ms,
    int valid,uint16_t lux,uint32_t current_millinits) {
    struct cab_action action={0,0,0};
    if (!p->enabled || !p->awake || !valid) {cab_reset_tracking(p);return action;}
    if (p->have_sample && now_ms<p->last_ms) {
        /* Reset on a clock discontinuity rather than unsigned underflow. */
        p->have_request=0; p->hold_until_ms=0; cab_reset_tracking(p);
    }
    if (p->have_request && cab_distance(current_millinits,p->requested)>1000u) {
        cab_manual(p,now_ms);return action;
    }
    if (now_ms<p->hold_until_ms) return action;
    const uint32_t value=(uint32_t)lux*1000u;
    if (!p->have_sample || now_ms-p->last_ms>CAB_MAX_GAP_MS) {
        p->filtered_millilux=value; p->last_ms=now_ms;
        p->have_sample=1; p->have_candidate=0; return action;
    }
    const uint64_t dt=now_ms-p->last_ms; p->last_ms=now_ms;
    const uint32_t tau=value>p->filtered_millilux?1000u:3000u;
    const int64_t delta=(int64_t)value-p->filtered_millilux;
    p->filtered_millilux=(uint32_t)((int64_t)p->filtered_millilux+delta*(int64_t)dt/(tau+dt));
    const uint32_t target=cab_target(p->filtered_millilux);
    const uint32_t threshold=current_millinits/20u>5000u?current_millinits/20u:5000u;
    if (cab_distance(target,current_millinits)<threshold) {p->have_candidate=0;return action;}
    if (!p->have_candidate ||
        ((p->candidate>current_millinits)!=(target>current_millinits)) ||
        cab_distance(target,p->candidate)>threshold) {
        p->candidate=target;p->candidate_ms=now_ms;p->have_candidate=1;return action;
    }
    const uint32_t dwell=target>current_millinits?1000u:5000u;
    if (now_ms-p->candidate_ms<dwell) return action;
    action.apply=1;action.millinits=cab_clamp(target);action.fade_ms=1000;
    return action;
}
static inline void cab_commit(struct cab_policy *p,const struct cab_action *a) {
    if (!a->apply) return;
    p->requested=a->millinits;p->have_request=1;p->have_candidate=0;
}
#endif
