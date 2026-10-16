// ps5-homebrew-ui - Design "PS5 Share Hub" (prototype): our capture gallery.
// Copyright (C) 2026 PS5 Share contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Prototype of the PS5 Share homebrew hub in the kit's own renderer:
// Aurora-style banner + sidebar + thumbnail shelf, Neon-style electric
// blue/purple glow, and a frosted-glass detail panel with a Share action.
// Capture art here is stand-in catalog covers; the integration uploads real
// decoded captures with GlBatch::create_texture() the same way.

#include "concepts/concepts.hpp"

#include "core/save_file.hpp"
#include "core/tween.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0xf2f4ff);
const Color kMuted = Color::rgb(0x8f9bd0);
const Color kBlue = Color::rgb(0x2f7cf6);
const Color kPurple = Color::rgb(0x8c46fa);
const Color kCyan = Color::rgb(0x5ad8ff);
const Color kGood = Color::rgb(0x59e682);

constexpr float kSide = 40.0f;
constexpr float kRailW = 268.0f;
constexpr float kRailX = 40.0f;
constexpr float kGridX = 360.0f;
constexpr float kTileW = 250.0f;
constexpr float kTileH = 141.0f;
constexpr float kTileGapX = 24.0f;
constexpr float kTilePitchY = 218.0f;
constexpr int kColumns = 3;
constexpr int kVisibleRows = 3; // rows on screen; the grid scrolls past this
constexpr float kPanelX = 1420.0f;
constexpr float kPanelW = 420.0f;

constexpr const char *kTechniques[] = {
    "Aurora shelf layout: banner, rail, thumbnail grid, frosted detail panel",
    "Neon glow: layered halo around the focus and the Share action",
    "SpringRect focus ring that glides between tiles",
    "Real textures in tiles via list.image (stand-in covers here)",
};

constexpr app::TourStep kTour[] = {
    // Captures are taken after the wait, before the press: every capture
    // gets its own step following the input it shows.
    {1.2f, 0, Direction::none, "gallery"},
    {0.4f, 0, Direction::right},
    {0.4f, 0, Direction::right},
    {0.3f, action_bit(Action::confirm)},
    {0.5f, 0, Direction::none, "detail"},
    // Single-design shell passes L1/R1 through to the hub tabs.
    {0.3f, action_bit(Action::page_next)},
    {0.5f, 0, Direction::none, "videos"},
    {0.3f, action_bit(Action::page_next)},
    {0.5f, 0, Direction::none, "settings"},
};

struct Capture
{
    const char *name;
    const char *meta; // date, format, size
    const char *format;
    bool latest;
};

constexpr Capture kShots[] = {
    {"20261015_101500_00077777.png", "Oct 15, 2026 \xC2\xB7 PNG \xC2\xB7 1.8 MB", "PNG", true},
    {"20261015_093012_00011111.jpg", "Oct 15, 2026 \xC2\xB7 JPG \xC2\xB7 1.1 MB", "JPG", false},
    {"20261014_221540_00022222.jpg", "Oct 14, 2026 \xC2\xB7 JPG \xC2\xB7 1.4 MB", "JPG", false},
    {"20261014_180301_00033333.jpg", "Oct 14, 2026 \xC2\xB7 JPG \xC2\xB7 1.6 MB", "JPG", false},
    {"20261013_120000_00044444.jpg", "Oct 13, 2026 \xC2\xB7 JPG \xC2\xB7 1.4 MB", "JPG", false},
    {"20261012_083015_00055555.jpg", "Oct 12, 2026 \xC2\xB7 JPG \xC2\xB7 1.3 MB", "JPG", false},
    {"20261011_235959_00066666.jpg", "Oct 11, 2026 \xC2\xB7 JPG \xC2\xB7 1.3 MB", "JPG", false},
};

constexpr Capture kClips[] = {
    {"20261015_080000_00088888.mp4", "Oct 15, 2026 \xC2\xB7 MP4 \xC2\xB7 48 MB", "MP4", true},
    {"20261014_193000_00099999.mp4", "Oct 14, 2026 \xC2\xB7 MP4 \xC2\xB7 122 MB", "MP4", false},
    {"20261012_140000_00101010.mp4", "Oct 12, 2026 \xC2\xB7 MP4 \xC2\xB7 8 MB", "MP4", false},
};

class ShareHub final : public app::Concept
{
  public:
    explicit ShareHub(app::Context &context) : context_(context)
    {
        for (const Capture &c : kShots)
            stub_photos_.push_back(
                {c.name, c.meta, c.format, "", false, c.latest, 0, 0, 0, false, ""});
        for (const Capture &c : kClips)
            stub_videos_.push_back(
                {c.name, c.meta, c.format, "", true, c.latest, 0, 0, 0, false, ""});
        ring_.snap(tile_rect(0));
        keyboard_.style.bindings = ui::KeyboardBindings::standard();
        keyboard_.style.max_length = 300;
        keyboard_.on_text = [this](std::string_view text) { webhook_edit_.append(text); };
        keyboard_.on_backspace = [this] {
            if (!webhook_edit_.empty())
                webhook_edit_.pop_back();
        };
    }

    // Real captures when the shell wired a media store, else demo stubs
    // (unit tests and the component gallery have no store). A wired-but-
    // empty store shows the real empty state (with the scan diagnosis),
    // never stubs: fake names masked the console scan failure for builds.
    const std::vector<share::Capture> &items(bool photos) const
    {
        if (context_.media != nullptr)
            return photos ? context_.media->photos() : context_.media->videos();
        return photos ? stub_photos_ : stub_videos_;
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "ps5share",
            "PS5 Share Hub",
            "Capture gallery prototype: rail, thumbnail shelf, frosted detail, Share",
            "src/concepts/ps5share.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x2f7cf6),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        if (!settings_ready_)
        {
            settings_ready_ = true;
            // Lazy: file access during startup crashed the console load;
            // hub time (after the first presented frames) is safe. The read
            // goes through the app's save:: helpers (bisect-proven safe).
            if (context_.share_settings != nullptr)
                share::load_settings(*context_.share_settings);
            // Where did the previous upload stop? The breadcrumb is
            // written before each step, so it survives the app closing.
            std::string crumb;
            if (save::read_file("/download0/PS5Share/ps5share-breadcrumb.txt", &crumb, 256))
            {
                while (!crumb.empty() &&
                       (crumb.back() == '\n' || crumb.back() == '\r' || crumb.back() == ' '))
                    crumb.pop_back();
                last_step_ = crumb;
            }
        }
        if (!init_status_)
        {
            init_status_ = true;
            status_ = initial_status();
        }
        if (init_stage_ >= 0)
        {
            // Stages run one per frame so the stage name ("Net: ...") was
            // painted before the call: a crash names its stage.
            static constexpr const char* kStageNames[] = {"SceNet library", "SceNet pool",
                                                          "TLS entropy", "TLS stack"};
            const int stage = init_stage_;
            const int code = share::net_init_stage(stage);
            const bool ok = stage == 3 ? code == 1 : code == 0;
            if (!ok)
            {
                init_stage_ = -1;
                start_pending_ = false;
                status_ = std::string("Network init failed: ") + kStageNames[stage] +
                          " (code " + std::to_string(code) + ").";
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            }
            else if (stage == 3)
            {
                net_ready_ = true;
                init_stage_ = -1;
                if (start_pending_)
                {
                    start_pending_ = false;
                    try_upload(feedback);
                }
                else if (test_pending_)
                {
                    test_pending_ = false;
                    try_test(feedback);
                }
            }
            else
            {
                init_stage_ = stage + 1;
                status_ = std::string("Net: ") + kStageNames[stage + 1] + "...";
            }
        }
        poll_upload();
        if (!editing_webhook_ &&
            (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev)))
        {
            const int next = (tab_ + (input.is_pressed(Action::page_next) ? 1 : 2)) % 3;
            tab_ = next;
            focus_ = 0;
            scroll_row_ = 0;
            selected_ = -1;
            share_focused_ = false;
            focus_refresh_ = false;
            ring_.snap(tile_rect(0));
            feedback.play(audio::Cue::tab);
        }
        if (tab_ == 2)
        {
            update_settings(input, dt, feedback);
            return;
        }
        if (share_focused_)
        {
            // Focus sits on the Share button: Up/Back returns to the grid,
            // Cross presses it.
            if (input.nav == Direction::up || input.is_pressed(Action::back))
            {
                share_focused_ = false;
                feedback.play(input.is_pressed(Action::back) ? audio::Cue::back
                                                            : audio::Cue::focus);
            }
            else if (input.is_pressed(Action::confirm))
            {
                try_upload(feedback);
            }
            ring_.target(share_rect());
            ring_.update(dt, 18.0f);
            return;
        }
        if (focus_refresh_)
        {
            // Focus sits on Refresh: Down/Back returns to the grid, Cross
            // rescans the capture folders.
            if (input.nav == Direction::down || input.is_pressed(Action::back))
            {
                focus_refresh_ = false;
                feedback.play(input.is_pressed(Action::back) ? audio::Cue::back
                                                            : audio::Cue::focus);
            }
            else if (input.is_pressed(Action::confirm))
            {
                refresh_media(feedback);
            }
            ring_.target(refresh_rect());
            ring_.update(dt, 18.0f);
            return;
        }
        const int count = static_cast<int>(items(tab_ == 0).size());
        // Keep every index inside the real list. An empty scan must never
        // index a capture (selecting from an empty gallery crashed here).
        if (count == 0)
        {
            focus_ = 0;
            selected_ = -1;
            share_focused_ = false;
        }
        else
        {
            if (focus_ >= count)
                focus_ = count - 1;
            if (selected_ >= count)
                selected_ = -1;
        }
        bool refused = false;
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int next = focus_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < count)
            {
                focus_ = next;
                feedback.play(audio::Cue::focus);
            }
            else
                refused = !input.nav_repeat;
            break;
        }
        case Direction::up:
        case Direction::down:
        {
            const int next = focus_ + (input.nav == Direction::down ? kColumns : -kColumns);
            if (input.nav == Direction::up && focus_ < kColumns &&
                context_.media != nullptr)
            {
                // Up from the first row lands on Refresh (works with an
                // empty gallery too: it is the rescan action).
                focus_refresh_ = true;
                feedback.play(audio::Cue::focus);
            }
            else if (next >= 0 && next < count)
            {
                focus_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (input.nav == Direction::down && selected_ >= 0 && next >= count)
            {
                // Down past the last row lands on the Share button.
                share_focused_ = true;
                feedback.play(audio::Cue::focus);
            }
            else
                refused = !input.nav_repeat;
            break;
        }
        case Direction::none:
            break;
        }
        if (refused)
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        if (input.is_pressed(Action::confirm) && count > 0)
        {
            selected_ = focus_;
            share_focused_ = false;
            feedback.play(audio::Cue::select);
        }
        if (input.is_pressed(Action::north))
            try_upload(feedback);
        if (input.is_pressed(Action::back) && selected_ >= 0)
        {
            selected_ = -1;
            status_ = "Selection cleared.";
            feedback.play(audio::Cue::back);
        }
        // Keep the focused row inside the visible window: the grid
        // scrolls under the focus ring.
        {
            const int rows = (count + kColumns - 1) / kColumns;
            const int max_scroll = rows > kVisibleRows ? rows - kVisibleRows : 0;
            const int focus_row = focus_ / kColumns;
            if (focus_row < scroll_row_)
                scroll_row_ = focus_row;
            else if (focus_row > scroll_row_ + kVisibleRows - 1)
                scroll_row_ = focus_row - (kVisibleRows - 1);
            if (scroll_row_ > max_scroll)
                scroll_row_ = max_scroll;
            if (scroll_row_ < 0)
                scroll_row_ = 0;
        }
        ring_.target(tile_rect(focus_));
        ring_.update(dt, 18.0f);
    }

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        frame.backdrop.colors[0] = Color::rgb(0x0b1026);
        frame.backdrop.colors[1] = Color::rgb(0x1b1f4a);
        frame.backdrop.colors[2] = Color::rgb(0x3a2a6e);
        frame.backdrop.colors[3] = Color::rgb(0x6a3ad0);
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in);
        draw_banner(list);
        draw_rail(list);
        if (tab_ == 2)
            draw_settings(list);
        else
        {
            draw_grid(list);
            draw_detail(frame);
        }
        draw_status(list);
        list.pop_opacity();
        if (!editing_webhook_)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Select"},
                                     {ui::Button::circle, "Back"},
                                     {ui::Button::l1, "Switch Tab", ui::Button::r1},
                                     {ui::Button::triangle, "Share"}};
            ui::draw_hints(list, context_.fonts, ui::GlyphStyle::dark(), hints, 4, 1824,
                           true);
        }
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    Rect tile_rect(int index) const
    {
        const int col = index % kColumns;
        const int row = index / kColumns;
        return {kGridX + col * (kTileW + kTileGapX),
                300.0f + (row - scroll_row_) * kTilePitchY, kTileW, kTileH};
    }

    // Settings buttons, matching draw_settings' layout.
    Rect edit_rect() const
    {
        return {kGridX + 32, 280 + 122, 230, 54};
    }

    Rect test_rect() const
    {
        return {kGridX + 32, 524 + 118, 230, 54};
    }

    // Detail-panel Share button, matching draw_detail's layout.
    Rect share_rect() const
    {
        return {kPanelX + 36, 180 + 458, kPanelW - 72, 62};
    }

    // Grid header Refresh button, matching draw_grid's layout.
    Rect refresh_rect() const
    {
        return {1180, 196, 150, 48};
    }

    void refresh_media(app::Feedback &feedback)
    {
        focus_refresh_ = false;
        if (context_.media == nullptr)
            return;
        context_.media->refresh();
        focus_ = 0;
        scroll_row_ = 0;
        selected_ = -1;
        share_focused_ = false;
        status_ = "Gallery refreshed. Newest first.";
        feedback.play(audio::Cue::select);
    }

    void update_settings(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        if (editing_webhook_)
        {
            const ui::Event event = keyboard_.handle(input, feedback);
            keyboard_.update(dt);
            if (event == ui::Event::activated)
                finish_edit(true, feedback);
            else if (event == ui::Event::cancelled)
                finish_edit(false, feedback);
            return;
        }
        if (input.nav == Direction::up || input.nav == Direction::left)
        {
            if (settings_focus_ != 0)
            {
                settings_focus_ = 0;
                feedback.play(audio::Cue::focus);
            }
        }
        else if (input.nav == Direction::down || input.nav == Direction::right)
        {
            if (settings_focus_ != 1)
            {
                settings_focus_ = 1;
                feedback.play(audio::Cue::focus);
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            if (settings_focus_ == 0)
                begin_edit(feedback);
            else
                try_test(feedback);
        }
    }

    void begin_edit(app::Feedback &feedback)
    {
        webhook_edit_ = context_.share_settings != nullptr
                            ? context_.share_settings->webhook_url
                            : std::string();
        keyboard_.set_bounds({360, 330, 1200, 520});
        keyboard_.set_length(static_cast<int>(webhook_edit_.size()));
        keyboard_.enter();
        editing_webhook_ = true;
        feedback.play(audio::Cue::select);
    }

    void finish_edit(bool save, app::Feedback &feedback)
    {
        editing_webhook_ = false;
        if (!save)
        {
            status_ = "Webhook edit cancelled.";
            feedback.play(audio::Cue::back);
            return;
        }
        std::string url = webhook_edit_;
        const auto space = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        while (!url.empty() && space(url.back()))
            url.pop_back();
        std::size_t lead = 0;
        while (lead < url.size() && space(url[lead]))
            ++lead;
        url.erase(0, lead);
        if (!share::is_allowed_webhook_url(url))
        {
            status_ = "That is not a valid Discord webhook URL.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        if (context_.share_settings == nullptr)
        {
            status_ = "Settings unavailable in this build.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        context_.share_settings->webhook_url = url;
        if (share::save_settings(*context_.share_settings))
        {
            status_ = "Webhook saved.";
            feedback.play(audio::Cue::select);
        }
        else
        {
            status_ = "Could not save the webhook file.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }

    void try_test(app::Feedback &feedback)
    {
        if (context_.uploader == nullptr)
        {
            status_ = "Uploads unavailable in this build.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        if (!net_ready_)
        {
            // Same staged init as uploads; the test runs when it lands.
            if (init_stage_ < 0)
            {
                init_stage_ = 0;
                test_pending_ = true;
                status_ = "Net: SceNet library...";
                feedback.play(audio::Cue::launch);
            }
            return;
        }
        if (context_.share_settings == nullptr ||
            context_.share_settings->webhook_url.empty())
        {
            status_ = "No webhook configured. Edit the webhook first.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        share::note_upload_step("test-pressed");
        if (!context_.uploader->startTestMessage(context_.share_settings->webhook_url))
        {
            status_ = context_.uploader->snapshot().active
                          ? "Upload already in progress."
                          : "Could not start the webhook test.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        upload_seen_ = false;
        result_.clear();
        status_ = "Sending webhook test...";
        feedback.play(audio::Cue::launch);
    }

    std::string initial_status() const
    {
        if (!last_step_.empty() && last_step_ != "done")
            return "Previous upload stopped at: " + last_step_ + ". Share again to retry.";
        if (context_.uploader == nullptr)
            return "Uploads unavailable in this build.";
        if (context_.share_settings == nullptr ||
            context_.share_settings->webhook_url.empty())
            return "No webhook configured. Open Settings to add one.";
        return "Gallery refreshed. Newest first.";
    }

    // Copies the worker-thread upload state into the frame and reports
    // finished uploads once, honestly: progress while active, the worker's
    // own message when it completes.
    void poll_upload()
    {
        if (context_.uploader == nullptr)
            return;
        snap_ = context_.uploader->snapshot();
        if (snap_.active)
        {
            unsigned pct = 0;
            if (snap_.totalBytes != 0)
                pct = static_cast<unsigned>(snap_.sentBytes * 100 / snap_.totalBytes);
            else if (snap_.fraction > 0.0)
                pct = static_cast<unsigned>(snap_.fraction * 100.0);
            if (pct > 100)
                pct = 100;
            char buf[192];
            if (snap_.note != nullptr)
            {
                // Worker milestone: painted every frame, so a crash
                // leaves the faulting upload step on screen.
                if (snap_.totalBytes != 0)
                    std::snprintf(buf, sizeof(buf), "%s %u%%", snap_.note, pct);
                else
                    std::snprintf(buf, sizeof(buf), "%s", snap_.note);
            }
            else
            {
                std::snprintf(buf, sizeof(buf), "Uploading %s %u%% ...",
                              snap_.fileName.c_str(), pct);
            }
            status_ = buf;
            result_.clear();
        }
        else if (snap_.complete && !upload_seen_)
        {
            upload_seen_ = true;
            if (snap_.succeeded)
            {
                const bool test = snap_.fileName == "Webhook test";
                status_ = test ? snap_.message : "Shared to Discord.";
                result_ = test ? snap_.message : "Shared to Discord.";
            }
            else
            {
                status_ = "Upload failed: " + snap_.message;
                result_ = "Upload failed.";
            }
        }
    }

    void try_upload(app::Feedback &feedback)
    {
        share_focused_ = false;
        if (selected_ < 0)
        {
            status_ = "Select a capture first.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        if (context_.uploader == nullptr)
        {
            status_ = "Uploads unavailable in this build.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        if (!net_ready_)
        {
            // Defer init stage by stage; each stage name is presented for
            // a full frame before its call runs.
            if (init_stage_ < 0)
            {
                init_stage_ = 0;
                start_pending_ = true;
                status_ = "Net: SceNet library...";
                feedback.play(audio::Cue::launch);
            }
            return;
        }
        if (context_.share_settings == nullptr ||
            context_.share_settings->webhook_url.empty())
        {
            status_ = "No webhook configured. Open Settings to add one.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        const std::vector<share::Capture> &caps = items(tab_ == 0);
        if (selected_ >= static_cast<int>(caps.size()))
        {
            status_ = "Selection cleared.";
            selected_ = -1;
            return;
        }
        const share::Capture &cap = caps[static_cast<std::size_t>(selected_)];
        share::note_upload_step("share-pressed");
        if (!context_.uploader->startFileUpload(context_.share_settings->webhook_url,
                                                cap.absolute_path))
        {
            status_ = context_.uploader->snapshot().active ? "Upload already in progress."
                                                           : "Could not start the upload.";
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }
        upload_seen_ = false;
        result_.clear();
        status_ = "Uploading " + cap.name + " ...";
        feedback.play(audio::Cue::launch);
    }

    void draw_banner(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        list.glow({kSide, 44, 330, 92}, 24, 30, kBlue.with_alpha(0.35f));
        ui::text(list, fonts.display, "PS5 SHARE HUB", kSide + 110, 108, 54, kInk);
        ui::text(list, fonts.semibold, "CAPTURE \xC2\xB7 SHARE \xC2\xB7 DISCORD", kSide + 112,
                 140, 19, kCyan, gfx::Align::left, 3.0f);
        const Rect pill{1420, 52, 460, 60};
        list.rounded_rect(pill, 30, Color::rgb(0x0d1530, 0.92f));
        list.bordered_rect(pill, 30, Color::rgb(0x000000, 0.0f), 2, kGood.with_alpha(0.7f));
        list.circle(pill.x + 34, pill.cy(), 9, kGood);
        ui::text(list, fonts.semibold, "Discord \xC2\xB7 Connected", pill.x + 54, pill.cy() + 9,
                 24, kInk);
    }

    void draw_rail(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const char *labels[] = {"Screenshots", "Videos", "Settings"};
        const std::string photos_n = std::to_string(items(true).size());
        const std::string videos_n = std::to_string(items(false).size());
        const char *counts[] = {photos_n.c_str(), videos_n.c_str(), ""};
        for (int i = 0; i < 3; ++i)
        {
            const Rect row{kRailX, 210.0f + i * 86, kRailW, 68};
            const bool active = i == tab_;
            if (active)
            {
                list.glow(row, 16, 22, kBlue.with_alpha(0.45f));
                list.gradient_rect_h(row, 16, kBlue, kPurple);
                list.bordered_rect(row.inset(-2), 18, Color::rgb(0x000000, 0.0f), 2,
                                   kCyan.with_alpha(0.8f));
            }
            else
            {
                list.rounded_rect(row, 16, Color::rgb(0x0e1430, 0.9f));
            }
            ui::text(list, fonts.semibold, labels[i], row.x + 24, row.cy() + 9, 26, kInk);
            if (counts[i][0] != '\0')
            {
                const Rect pill{row.x + row.w - 58, row.cy() - 16, 42, 32};
                list.rounded_rect(pill, 16,
                                  active ? Color::rgb(0x0a1024, 0.65f)
                                         : Color::rgb(0x28396e, 0.9f));
                ui::text(list, fonts.semibold, counts[i], pill.cx(), pill.cy() + 9, 22,
                         kCyan, gfx::Align::center);
            }
        }
        ui::text(list, fonts.semibold, "DISCORD", kRailX + 4, 830, 18, kMuted,
                 gfx::Align::left, 3.0f);
        list.circle(kRailX + 12, 862, 8, kGood);
        ui::text(list, fonts.semibold, "Connected", kRailX + 30, 871, 24, kGood);
    }

    void draw_grid(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const bool photos = tab_ == 0;
        const std::vector<share::Capture> &caps = items(photos);
        const int count = static_cast<int>(caps.size());
        ui::text(list, fonts.semibold, photos ? "Screenshots" : "Videos", kGridX, 232, 44,
                 kInk);
        char sub[192];
        if (!photos && context_.media != nullptr)
            std::snprintf(sub, sizeof(sub), "%d videos \xC2\xB7 Newest first \xC2\xB7 thumbs %s",
                          count, context_.media->thumbnail_info().c_str());
        else if (context_.media != nullptr &&
            (context_.media->pending_tiles() != 0 || context_.media->failed_tiles() != 0))
            std::snprintf(sub, sizeof(sub), "%d %s \xC2\xB7 Newest first \xC2\xB7 %zu decoding, %zu no preview",
                          count, photos ? "screenshots" : "videos",
                          context_.media->pending_tiles(), context_.media->failed_tiles());
        else
            std::snprintf(sub, sizeof(sub), "%d %s \xC2\xB7 Newest first", count,
                          photos ? "screenshots" : "videos");
        const std::string sub_fit = fonts.regular.font->fit(sub, 24, 940);
        ui::text(list, fonts.regular, sub_fit.c_str(), kGridX, 268, 24, kMuted);
        const Rect refresh{1180, 196, 150, 48};
        if (focus_refresh_)
            list.glow(refresh, 14, 20, kCyan.with_alpha(0.5f));
        list.bordered_rect(refresh, 14, Color::rgb(0x141c44, 0.95f), 2,
                           kBlue.with_alpha(0.8f));
        if (focus_refresh_)
            list.bordered_rect(refresh.inset(-3), 16, Color::rgb(0x000000, 0.0f), 3,
                               kWhite);
        ui::text(list, fonts.semibold, "Refresh", refresh.cx(), refresh.cy() + 9, 24, kInk,
                 gfx::Align::center);
        if (caps.empty())
        {
            ui::text(list, fonts.semibold, photos ? "No screenshots yet" : "No videos yet",
                     kGridX, 340, 30, kInk);
            ui::paragraph(list, fonts.regular,
                          "Capture media on the console, then restart the app. It appears here, newest first.",
                          kGridX, 384, 24, 700, 32, kMuted, 3);
            if (context_.media != nullptr)
                ui::paragraph(list, fonts.regular, context_.media->diagnosis().c_str(),
                              kGridX, 500, 22, 700, 30, kMuted, 3);
            return;
        }
        const int rows = (count + kColumns - 1) / kColumns;
        const int first = scroll_row_ * kColumns;
        const int last = std::min(count, first + kVisibleRows * kColumns);
        for (int i = first; i < last; ++i)
        {
            const Rect tile = tile_rect(i);
            const float fade = tween::stagger(age_, 2 + i, 0.05f, 0.4f);
            list.push_opacity(fade);
            const share::Capture &cap = caps[static_cast<std::size_t>(i)];
            list.shadow({tile.x, tile.y + 8, tile.w, tile.h}, 14, 22,
                        Color::rgb(0x000000, 0.55f));
            if (cap.texture != 0)
            {
                list.image(cap.texture, tile, gfx::kFullUv, kWhite, 14);
            }
            else if (cap.video)
            {
                list.gradient_rect(tile, 14, Color::rgb(0x2c3a8c), Color::rgb(0x0c1030));
                const Rect play{tile.cx() - 20, tile.cy() - 24, 40, 48};
                list.triangle(play, kWhite.with_alpha(0.9f), 0.0f, 1.5708f);
            }
            else
            {
                list.gradient_rect(tile, 14, Color::rgb(0x1c4a8c), Color::rgb(0x0c1030));
            }
            list.bordered_rect(tile, 14, Color::rgb(0x000000, 0.0f), 1.5f,
                               kCyan.with_alpha(i == focus_ ? 0.0f : 0.25f));
            if (cap.latest)
            {
                const Rect pill{tile.x + 12, tile.y + 12, 96, 30};
                list.rounded_rect(pill, 15, kPurple.with_alpha(0.92f));
                ui::text(list, fonts.semibold, "LATEST", pill.cx(), pill.cy() + 8, 18, kWhite,
                         gfx::Align::center);
            }
            const float tag_w =
                fonts.semibold.measure(cap.format.c_str(), 18) + 24.0f;
            const Rect tag{tile.x + tile.w - tag_w - 10, tile.y + tile.h - 40, tag_w, 30};
            list.rounded_rect(tag, 15, Color::rgb(0x060a1a, 0.78f));
            ui::text(list, fonts.semibold, cap.format.c_str(), tag.cx(), tag.cy() + 8, 18,
                     kCyan, gfx::Align::center);
            ui::text(list, fonts.regular, fonts.regular.font->fit(cap.name, 20, kTileW),
                     tile.x, tile.y + tile.h + 30, 20, kInk);
            const std::string meta_fit =
                fonts.regular.font->fit(cap.meta.c_str(), 20, kTileW + kTileGapX - 4);
            ui::text(list, fonts.regular, meta_fit.c_str(), tile.x, tile.y + tile.h + 56,
                     20, kMuted);
            if (cap.preview_failed)
                ui::text(list, fonts.regular, "No preview", tile.x, tile.y + tile.h + 82,
                         20, kMuted);
            list.pop_opacity();
        }
        if (rows > kVisibleRows)
        {
            const float track_h =
                kVisibleRows * kTilePitchY - (kTilePitchY - kTileH);
            const Rect track{1210, 300, 8, track_h};
            list.rounded_rect(track, 4, Color::rgb(0x28396e, 0.55f));
            const float content_h = rows * kTilePitchY - (kTilePitchY - kTileH);
            const float thumb_h = track_h * track_h / content_h;
            const int max_scroll = rows - kVisibleRows;
            const float thumb_y =
                300.0f + (track_h - thumb_h) *
                             (max_scroll > 0
                                  ? static_cast<float>(scroll_row_) / max_scroll
                                  : 0.0f);
            list.rounded_rect({1210, thumb_y, 8, thumb_h}, 4, kCyan.with_alpha(0.85f));
        }
        const Rect ring = ring_.value();
        list.glow(ring.inset(-8), 18, 26, kCyan.with_alpha(0.5f));
        list.glow(ring.inset(-3), 16, 12, kPurple.with_alpha(0.5f));
        list.bordered_rect(ring, 16, Color::rgb(0x000000, 0.0f), 3, kWhite);
    }

    void draw_detail(app::Frame &frame) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const bool photos = tab_ == 0;
        const std::vector<share::Capture> &caps = items(photos);
        const int count = static_cast<int>(caps.size());
        const Rect panel{kPanelX, 180, kPanelW, 700};
        frame.glass = true;
        frame.overlay.glass(frame.glass_texture, panel, 24, kWhite);
        frame.overlay.rounded_rect(panel, 24, Color::rgb(0x0d1330, 0.62f));
        frame.overlay.bordered_rect(panel, 24, Color::rgb(0x000000, 0.0f), 2,
                                    kCyan.with_alpha(0.35f));
        gfx::DrawList &over = frame.overlay;
        if (selected_ < 0 || selected_ >= count)
        {
            ui::text(over, fonts.semibold, "Nothing selected", panel.x + 36, panel.y + 64,
                     30, kInk);
            ui::paragraph(over, fonts.regular,
                          "Pick any capture on the left to preview it here, then share it to Discord.",
                          panel.x + 36, panel.y + 110, 24, 348, 34, kMuted, 4);
            return;
        }
        const share::Capture &cap = caps[static_cast<std::size_t>(selected_)];
        char pos[32];
        std::snprintf(pos, sizeof(pos), "%d / %d", selected_ + 1, count);
        ui::text(over, fonts.regular, pos, panel.x + 36, panel.y + 58, 24, kMuted);
        const Rect art_rect{panel.x + 36, panel.y + 84, panel.w - 72, 209};
        over.glow(art_rect.inset(10), 18, 30, kPurple.with_alpha(0.35f));
        if (cap.texture != 0)
            over.image(cap.texture, art_rect, gfx::kFullUv, kWhite, 16);
        else if (cap.video)
        {
            over.gradient_rect(art_rect, 16, Color::rgb(0x2c3a8c), Color::rgb(0x0c1030));
            const Rect play{art_rect.cx() - 26, art_rect.cy() - 32, 52, 64};
            over.triangle(play, kWhite.with_alpha(0.9f), 0.0f, 1.5708f);
        }
        else
            over.gradient_rect(art_rect, 16, Color::rgb(0x1c4a8c), Color::rgb(0x0c1030));
        const std::string detail_name =
            fonts.semibold.font->fit(cap.name, 25, panel.w - 72);
        ui::text(over, fonts.semibold, detail_name.c_str(), panel.x + 36, panel.y + 330, 25,
                 kInk);
        if (cap.latest)
        {
            const Rect pill{panel.x + 36, panel.y + 366, 210, 32};
            over.rounded_rect(pill, 16, kPurple.with_alpha(0.92f));
            ui::text(over, fonts.semibold, "LATEST CAPTURE", pill.cx(), pill.cy() + 8, 18,
                     kWhite, gfx::Align::center);
        }
        ui::text(over, fonts.regular, cap.meta.c_str(), panel.x + 36, panel.y + 414, 23,
                 kMuted);
        const Rect share{panel.x + 36, panel.y + 458, panel.w - 72, 62};
        const bool uploading = snap_.active;
        unsigned pct = 0;
        if (uploading)
        {
            if (snap_.totalBytes != 0)
                pct = static_cast<unsigned>(snap_.sentBytes * 100 / snap_.totalBytes);
            else if (snap_.fraction > 0.0)
                pct = static_cast<unsigned>(snap_.fraction * 100.0);
            if (pct > 100)
                pct = 100;
        }
        char share_label[64];
        if (uploading)
            std::snprintf(share_label, sizeof(share_label), "Uploading %u%% ...", pct);
        else
            std::snprintf(share_label, sizeof(share_label), "Share to Discord");
        over.glow(share, 18, 22,
                  (share_focused_ ? kCyan : kBlue).with_alpha(share_focused_ ? 0.8f : 0.5f));
        over.gradient_rect_h(share, 18, kBlue, kPurple);
        if (uploading)
        {
            const Rect fill{share.x, share.y, share.w * pct / 100, share.h};
            over.rounded_rect(fill, 18, kWhite.with_alpha(0.22f));
        }
        if (share_focused_)
            over.bordered_rect(share.inset(-3), 20, Color::rgb(0x000000, 0.0f), 3, kWhite);
        ui::text(over, fonts.semibold, share_label, share.cx(), share.cy() + 10, 28, kWhite,
                 gfx::Align::center);
        float rows_y = panel.y + 556;
        if (!result_.empty())
        {
            const bool good = result_ == "Shared to Discord.";
            ui::text(over, fonts.semibold, result_.c_str(), panel.x + 36, panel.y + 550, 23,
                     good ? kGood : kMuted);
            rows_y = panel.y + 586;
        }
        ui::text(over, fonts.regular, "Name", panel.x + 36, rows_y, 22, kMuted);
        const std::string row_name =
            fonts.regular.font->fit(cap.name, 22, panel.w - 166);
        ui::text(over, fonts.regular, row_name.c_str(), panel.x + 130, rows_y, 22, kInk);
        ui::text(over, fonts.regular, "Size", panel.x + 36, rows_y + 30, 22, kMuted);
        ui::text(over, fonts.regular, cap.meta.c_str(), panel.x + 130, rows_y + 30, 22,
                 kInk);
        float note_y = rows_y + 60;
        if (cap.duration_seconds >= 0.0)
        {
            const std::string length = share::format_duration(cap.duration_seconds);
            ui::text(over, fonts.regular, "Length", panel.x + 36, note_y, 22, kMuted);
            ui::text(over, fonts.regular, length.c_str(), panel.x + 130, note_y, 22, kInk);
            note_y += 30;
        }
        if (cap.width > 0 && cap.height > 0)
        {
            char dims[40];
            std::snprintf(dims, sizeof(dims), "%d \xC3\x97 %d", cap.width, cap.height);
            ui::text(over, fonts.regular, "Video", panel.x + 36, note_y, 22, kMuted);
            ui::text(over, fonts.regular, dims, panel.x + 130, note_y, 22, kInk);
            note_y += 30;
        }
        note_y += 10;
        if (cap.preview_failed)
        {
            const std::string why =
                fonts.regular.font->fit(cap.preview_error.c_str(), 22, panel.w - 166);
            ui::text(over, fonts.regular, "Preview", panel.x + 36, note_y, 22, kMuted);
            ui::text(over, fonts.regular, why.c_str(), panel.x + 130, note_y, 22, kInk);
            note_y += 30;
        }
        ui::paragraph(over, fonts.regular, "Original media stays read-only.", panel.x + 36,
                      note_y, 22, 348, 30, kMuted, 2);
    }

    void draw_settings_button(gfx::DrawList &list, const Rect &rect, const char *label,
                              bool focused) const
    {
        const ui::Fonts &fonts = context_.fonts;
        if (focused)
            list.glow(rect, 14, 20, kCyan.with_alpha(0.5f));
        list.bordered_rect(rect, 14, Color::rgb(0x141c44, 0.95f), 2,
                           kBlue.with_alpha(0.8f));
        if (focused)
            list.bordered_rect(rect.inset(-3), 16, Color::rgb(0x000000, 0.0f), 3, kWhite);
        ui::text(list, fonts.semibold, label, rect.cx(), rect.cy() + 9, 24, kInk,
                 gfx::Align::center);
    }

    void draw_settings(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        ui::text(list, fonts.semibold, "Settings", kGridX, 232, 44, kInk);
        const Rect card{kGridX, 280, 970, 220};
        list.rounded_rect(card, 20, Color::rgb(0x0e1430, 0.92f));
        list.bordered_rect(card, 20, Color::rgb(0x000000, 0.0f), 1.5f,
                           kBlue.with_alpha(0.4f));
        ui::text(list, fonts.semibold, "Discord webhook", card.x + 32, card.y + 52, 28,
                 kInk);
        ui::text(list, fonts.regular,
                 "Uploads go to one Discord webhook. It is stored only on this console.",
                 card.x + 32, card.y + 92, 23, kMuted);
        const Rect edit = edit_rect();
        draw_settings_button(list, edit, "Edit webhook",
                             settings_focus_ == 0 && !editing_webhook_);
        const bool webhook_ok = context_.share_settings != nullptr &&
                                !context_.share_settings->webhook_url.empty();
        std::string state = webhook_ok ? "Configured" : "Not configured";
        if (webhook_ok)
        {
            const std::string &url = context_.share_settings->webhook_url;
            state += " \xC2\xB7 ends \xE2\x80\xA6" +
                     (url.size() > 4 ? url.substr(url.size() - 4) : url);
        }
        ui::text(list, fonts.semibold, state.c_str(), edit.x + edit.w + 28, edit.cy() + 9,
                 24, webhook_ok ? kGood : kMuted);
        const Rect test{kGridX, 524, 970, 190};
        list.rounded_rect(test, 20, Color::rgb(0x0e1430, 0.92f));
        list.bordered_rect(test, 20, Color::rgb(0x000000, 0.0f), 1.5f,
                           kBlue.with_alpha(0.4f));
        ui::text(list, fonts.semibold, "Connection test", test.x + 32, test.y + 52, 28,
                 kInk);
        ui::text(list, fonts.regular, "Posts a short test message to the webhook channel.",
                 test.x + 32, test.y + 88, 23, kMuted);
        draw_settings_button(list, test_rect(), "Test webhook",
                             settings_focus_ == 1 && !editing_webhook_);
        if (editing_webhook_)
            draw_editor(list);
    }

    // Full-screen webhook editor: current URL (tail visible) over the
    // kit's controller keyboard. Done saves, Circle cancels.
    void draw_editor(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        list.gradient_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x04060f, 0.88f),
                           Color::rgb(0x04060f, 0.88f));
        const Rect panel{300, 100, 1320, 880};
        list.rounded_rect(panel, 24, Color::rgb(0x0d1330, 0.98f));
        list.bordered_rect(panel, 24, Color::rgb(0x000000, 0.0f), 2,
                           kCyan.with_alpha(0.4f));
        ui::text(list, fonts.semibold, "Discord webhook URL", panel.x + 40, panel.y + 62,
                 30, kInk);
        if (webhook_edit_.empty())
        {
            ui::text(list, fonts.regular, "https://discord.com/api/webhooks/...",
                     panel.x + 40, panel.y + 118, 24, kMuted);
        }
        else
        {
            // Show the tail: that is where long URLs differ.
            std::string shown = webhook_edit_;
            while (shown.size() > 1 &&
                   fonts.regular.measure(shown.c_str(), 24) > panel.w - 80)
                shown.erase(shown.begin());
            if (shown.size() != webhook_edit_.size())
                shown = "\xE2\x80\xA6" + shown;
            ui::text(list, fonts.regular, shown.c_str(), panel.x + 40, panel.y + 118, 24,
                     kCyan);
        }
        ui::Canvas canvas{list, fonts, 0, clock_};
        keyboard_.draw(canvas);
        ui::text(list, fonts.regular,
                 "Type the full webhook URL, then choose Done to save. Circle cancels.",
                 panel.x + 40, panel.y + panel.h - 30, 22, kMuted);
    }

    void draw_status(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        // Upload results carry the worker's full message: fit it to the line.
        const std::string line = fonts.regular.font->fit(status_.c_str(), 24, 1060);
        ui::text(list, fonts.regular, line.c_str(), kGridX, 972, 24, kMuted);
    }

    app::Context &context_;
    std::vector<share::Capture> stub_photos_;
    std::vector<share::Capture> stub_videos_;
    int tab_ = 0;
    int focus_ = 0;
    int scroll_row_ = 0;
    int settings_focus_ = 0; // 0 = Edit webhook, 1 = Test webhook
    bool editing_webhook_ = false;
    bool test_pending_ = false;
    std::string webhook_edit_;
    ui::Keyboard keyboard_;
    int selected_ = -1;
    bool share_focused_ = false;
    bool focus_refresh_ = false;
    bool settings_ready_ = false;
    std::string last_step_;
    bool net_ready_ = false;
    int init_stage_ = -1;
    bool start_pending_ = false;
    bool upload_seen_ = true;
    bool init_status_ = false;
    share::UploadSnapshot snap_;
    std::string result_;
    std::string status_ = "Gallery refreshed. Newest first.";
    float age_ = 0.0f;
    float clock_ = 0.0f;
    ui::SpringRect ring_;
};

} // namespace

std::unique_ptr<app::Concept> make_ps5share(app::Context &context)
{
    return std::make_unique<ShareHub>(context);
}

} // namespace hui::concepts
