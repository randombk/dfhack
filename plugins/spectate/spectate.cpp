//
// Created by josh on 7/28/21.
//

#include "pause.h"

#include "Core.h"
#include <modules/Gui.h>
#include <Console.h>
#include <Export.h>
#include <PluginManager.h>
#include <modules/World.h>
#include <modules/EventManager.h>
#include <modules/Job.h>
#include <modules/Units.h>
#include <df/job.h>
#include <df/unit.h>
#include <df/historical_figure.h>
#include <df/global_objects.h>
#include <df/world.h>
#include <df/viewscreen.h>

#include <map>
#include <set>
#include <random>
#include <cinttypes>

DFHACK_PLUGIN("spectate");
DFHACK_PLUGIN_IS_ENABLED(enabled);
REQUIRE_GLOBAL(world);
REQUIRE_GLOBAL(ui);
REQUIRE_GLOBAL(pause_state);
REQUIRE_GLOBAL(d_init);

using namespace DFHack;
using namespace Pausing;
using namespace df::enums;

void onTick(color_ostream& out, void* tick);
void onJobStart(color_ostream &out, void* job);
void onJobCompletion(color_ostream &out, void* job);

uint64_t tick_threshold = 1000;
bool focus_jobs_enabled = false;
bool disengage_enabled = false;
bool unpause_enabled = false;
Pausing::AnnouncementLock* pause_lock = nullptr;
bool lock_collision = false;
bool announcements_disabled = false;

bool following_dwarf = false;
df::unit* our_dorf = nullptr;
df::job* job_watched = nullptr;
int32_t timestamp = -1;

std::set<int32_t> job_tracker;
std::map<uint16_t,int16_t> freq;
std::default_random_engine RNG;

void enable_auto_unpause(color_ostream &out, bool state);


#define base 0.99

static const std::string CONFIG_KEY = std::string(plugin_name) + "/config";
enum ConfigData {
    UNPAUSE,
    DISENGAGE,
    JOB_FOCUS,
    TICK_THRESHOLD
};

static PersistentDataItem config;
inline void saveConfig() {
    if (config.isValid()) {
        config.ival(UNPAUSE) = unpause_enabled;
        config.ival(DISENGAGE) = disengage_enabled;
        config.ival(JOB_FOCUS) = focus_jobs_enabled;
        config.ival(TICK_THRESHOLD) = tick_threshold;
    }
}

command_result spectate (color_ostream &out, std::vector <std::string> & parameters);

DFhackCExport command_result plugin_init (color_ostream &out, std::vector <PluginCommand> &commands) {
    commands.push_back(PluginCommand("spectate",
                                     "Automated spectator mode.",
                                     spectate,
                                     false));
    pause_lock = new AnnouncementLock("spectate");
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown (color_ostream &out) {
    delete pause_lock;
    return CR_OK;
}

DFhackCExport command_result plugin_load_data (color_ostream &out) {
    config = World::GetPersistentData(CONFIG_KEY);

    if (!config.isValid()) {
        config = World::AddPersistentData(CONFIG_KEY);
        saveConfig();
    } else {
        unpause_enabled = config.ival(UNPAUSE);
        disengage_enabled = config.ival(DISENGAGE);
        focus_jobs_enabled = config.ival(JOB_FOCUS);
        tick_threshold = config.ival(TICK_THRESHOLD);
        pause_lock->unlock();
        enable_auto_unpause(out, unpause_enabled);
    }
    return DFHack::CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    namespace EM = EventManager;
    if (enable && !enabled) {
        out.print("Spectate mode enabled!\n");
        using namespace EM::EventType;
        EM::EventHandler ticking(onTick, 15);
        EM::EventHandler start(onJobStart, 0);
        EM::EventHandler complete(onJobCompletion, 0);
        EM::registerListener(EventType::TICK, ticking, plugin_self);
        EM::registerListener(EventType::JOB_STARTED, start, plugin_self);
        EM::registerListener(EventType::JOB_COMPLETED, complete, plugin_self);
        enabled = true; // enable_auto_unpause won't do anything without this set now
        enable_auto_unpause(out, unpause_enabled);
    } else if (!enable && enabled) {
        // warp 8, engage!
        out.print("Spectate mode disabled!\n");
        EM::unregisterAll(plugin_self);
        // we need to retain whether auto-unpause is enabled, but we also need to disable its effect
        bool temp = unpause_enabled;
        enable_auto_unpause(out, false);
        unpause_enabled = temp;
        job_tracker.clear();
        freq.clear();
    }
    enabled = enable;
    return DFHack::CR_OK;
}

DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event) {
    if (enabled) {
        switch (event) {
            case SC_MAP_UNLOADED:
            case SC_BEGIN_UNLOAD:
            case SC_WORLD_UNLOADED:
                our_dorf = nullptr;
                job_watched = nullptr;
                following_dwarf = false;
            default:
                break;
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    // keeps announcement pause settings locked
    World::Update(); // from pause.h
    if (lock_collision) {
        if (unpause_enabled) {
            // player asked for auto-unpause enabled
            World::SaveAnnouncementSettings();
            if (World::DisableAnnouncementPausing()) {
                // now that we've got what we want, we can lock it down
                lock_collision = false;
            }
        } else {
            if (World::RestoreAnnouncementSettings()) {
                lock_collision = false;
            }
        }
    }
    int failsafe = 0;
    while (unpause_enabled && !world->status.popups.empty() && ++failsafe <= 10) {
        // dismiss announcement popup(s)
        Gui::getCurViewscreen(true)->feed_key(interface_key::CLOSE_MEGA_ANNOUNCEMENT);
        if (World::ReadPauseState()) {
            // WARNING: This has a possibility of conflicting with `reveal hell` - if Hermes himself runs `reveal hell` on precisely the right moment that is
            World::SetPauseState(false);
        }
    }
    if (failsafe >= 10) {
        out.printerr("spectate encountered a problem dismissing a popup!\n");
    }
    if (disengage_enabled && !World::ReadPauseState()) {
        if (our_dorf && our_dorf->id != df::global::ui->follow_unit) {
            plugin_enable(out, false);
        }
    }
    return DFHack::CR_OK;
}

void enable_auto_unpause(color_ostream &out, bool state) {
    // we don't need to do any of this yet if the plugin isn't enabled
    if (enabled) {
        // todo: R.E. UNDEAD_ATTACK event [still pausing regardless of announcement settings]
        // lock_collision == true means: enable_auto_unpause() was already invoked and didn't complete
        // The onupdate function above ensure the procedure properly completes, thus we only care about
        // state reversal here ergo `enabled != state`
        if (lock_collision && unpause_enabled != state) {
            out.print("handling collision\n");
            // if unpaused_enabled is true, then a lock collision means: we couldn't save/disable the pause settings,
            // therefore nothing to revert and the lock won't even be engaged (nothing to unlock)
            lock_collision = false;
            unpause_enabled = state;
            if (unpause_enabled) {
                // a collision means we couldn't restore the pause settings, therefore we only need re-engage the lock
                pause_lock->lock();
            }
            return;
        }
        // update the announcement settings if we can
        if (state) {
            if (World::SaveAnnouncementSettings()) {
                World::DisableAnnouncementPausing();
                announcements_disabled = true;
                pause_lock->lock();
            } else {
                out.printerr("lock collision enabling auto-unpause\n");
                lock_collision = true;
            }
        } else {
            pause_lock->unlock();
            if (announcements_disabled) {
                if (!World::RestoreAnnouncementSettings()) {
                    // this in theory shouldn't happen, if others use the lock like we do in spectate
                    out.printerr("lock collision disabling auto-unpause\n");
                    lock_collision = true;
                } else {
                    announcements_disabled = false;
                }
            }
        }
        if (lock_collision) {
            out.printerr(
                    "auto-unpause: must wait for another Pausing::AnnouncementLock to be lifted.\n"
                    "The action you were attempting will complete when the following lock or locks lift.\n");
            pause_lock->reportLocks(out);
        }
    }
    unpause_enabled = state;
}

command_result spectate (color_ostream &out, std::vector <std::string> & parameters) {
    if (!parameters.empty()) {
        if (parameters.size() >= 2 && parameters.size() <= 3) {
            bool state =false;
            bool set = false;
            if (parameters[0] == "enable") {
                state = true;
            } else if (parameters[0] == "disable") {
                state = false;
            } else if (parameters[0] == "set") {
                set = true;
            } else {
                return DFHack::CR_WRONG_USAGE;
            }
            if(parameters[1] == "auto-unpause"){
                enable_auto_unpause(out, state);
            } else if (parameters[1] == "auto-disengage") {
                disengage_enabled = state;
            } else if (parameters[1] == "focus-jobs") {
                focus_jobs_enabled = state;
            } else if (parameters[1] == "tick-threshold" && set && parameters.size() == 3) {
                try {
                    tick_threshold = std::abs(std::stol(parameters[2]));
                } catch (const std::exception &e) {
                    out.printerr("%s\n", e.what());
                }
            } else {
                return DFHack::CR_WRONG_USAGE;
            }
        }
    } else {
        out.print("Spectate is %s\n", enabled ? "ENABLED." : "DISABLED.");
        out.print("tick-threshold: %" PRIu64 "\n", tick_threshold);
        out.print("focus-jobs: %s\n", focus_jobs_enabled ? "on." : "off.");
        out.print("auto-unpause: %s\n", unpause_enabled ? "on." : "off.");
        out.print("auto-disengage: %s\n", disengage_enabled ? "on." : "off.");
    }
    saveConfig();
    return DFHack::CR_OK;
}

// every tick check whether to decide to follow a dwarf
void onTick(color_ostream& out, void* ptr) {
    int32_t tick = df::global::world->frame_counter;
    if (our_dorf) {
        if (!Units::isAlive(our_dorf)) {
            following_dwarf = false;
            df::global::ui->follow_unit = -1;
        }
    }
    if (!following_dwarf || (focus_jobs_enabled && !job_watched) || (tick - timestamp) > (int32_t) tick_threshold) {
        std::vector<df::unit*> dwarves;
        for (auto unit: df::global::world->units.active) {
            if (!Units::isCitizen(unit)) {
                continue;
            }
            dwarves.push_back(unit);
        }
        std::uniform_int_distribution<uint64_t> follow_any(0, dwarves.size() - 1);
        // if you're looking at a warning about a local address escaping, it means the unit* from dwarves (which aren't local)
        our_dorf = dwarves[follow_any(RNG)];
        df::global::ui->follow_unit = our_dorf->id;
        job_watched = our_dorf->job.current_job;
        following_dwarf = true;
        if (!job_watched) {
            timestamp = tick;
        }
    }
}

// every new worked job needs to be considered
void onJobStart(color_ostream& out, void* job_ptr) {
    // todo: detect mood jobs
    int32_t tick = df::global::world->frame_counter;
    auto job = (df::job*) job_ptr;
    // don't forget about it
    int zcount = ++freq[job->pos.z];
    job_tracker.emplace(job->id);
    // if we're not doing anything~ then let's pick something
    if ((focus_jobs_enabled && !job_watched) || (tick - timestamp) > (int32_t) tick_threshold) {
        following_dwarf = true;
        // todo: allow the user to configure b, and also revise the math
        const double b = base;
        double p = b * ((double) zcount / job_tracker.size());
        std::bernoulli_distribution follow_job(p);
        if (!job->flags.bits.special && follow_job(RNG)) {
            job_watched = job;
            if (df::unit* unit = Job::getWorker(job)) {
                our_dorf = unit;
                df::global::ui->follow_unit = unit->id;
            }
        } else {
            timestamp = tick;
            std::vector<df::unit*> nonworkers;
            for (auto unit: df::global::world->units.active) {
                if (!Units::isCitizen(unit) || unit->job.current_job) {
                    continue;
                }
                nonworkers.push_back(unit);
            }
            std::uniform_int_distribution<> follow_drunk(0, nonworkers.size() - 1);
            df::global::ui->follow_unit = nonworkers[follow_drunk(RNG)]->id;
        }
    }
}

// every job completed can be forgotten about
void onJobCompletion(color_ostream &out, void* job_ptr) {
    auto job = (df::job*) job_ptr;
    // forget about it
    freq[job->pos.z]--;
    freq[job->pos.z] = freq[job->pos.z] < 0 ? 0 : freq[job->pos.z];
    // the job doesn't exist, so we definitely need to get rid of that
    job_tracker.erase(job->id);
    // the event manager clones jobs and returns those clones for completed jobs. So the pointers won't match without a refactor of EM passing clones to both events
    if (job_watched && job_watched->id == job->id) {
        job_watched = nullptr;
    }
}