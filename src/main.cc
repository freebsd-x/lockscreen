// xlockscreen — Full-screen lock screen for FreeBSD-X (core session component,
// spawned by LockService — not a user app bundle)
//
// Launched automatically by LockService when the screen locks (idle timeout,
// suspend, or explicit lock request). Runs as a trusted overlay (is_shell=true
// gives it the highest compositor priority). Sends LockOpcode::Register so the
// compositor routes all input exclusively to this surface while locked.
//
// Authentication: sends LockOpcode::Unlock with the current username + typed
// password; the compositor calls pam_authenticate() and responds with LockState.
// On success (LockState.locked_ == 0), the app exits cleanly.

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <ctime>
#include <format>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Helpers ────────────────────────────────────────────────────────────────────

static std::string currentTime() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

static std::string currentDate() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%A, %B %e", &tm);
    return buf;
}

static const char* currentUser() {
    const char* u = ::getenv("USER");
    return u ? u : "user";
}

// ── App ─────────────────────────────────────────────────────────────────────────

int main(int /*argc*/, char** /*argv*/) {
    // ── Clock & date labels ────────────────────────────────────────────────────
    auto clock_w = std::make_shared<TextWidget>(currentTime(), 72.0f);
    clock_w->setColor(Color{1.0f, 1.0f, 1.0f, 1.0f});
    clock_w->setAlignment(Alignment::Center);

    auto date_w = std::make_shared<TextWidget>(currentDate(), 20.0f);
    date_w->setColor(Color{0.8f, 0.8f, 0.8f, 1.0f});
    date_w->setAlignment(Alignment::Center);

    // ── User label ────────────────────────────────────────────────────────────
    auto user_w = std::make_shared<TextWidget>(
        std::string(currentUser()), 16.0f);
    user_w->setColor(Color{0.85f, 0.85f, 0.85f, 1.0f});
    user_w->setAlignment(Alignment::Center);

    // ── Password field ────────────────────────────────────────────────────────
    auto pw_input = std::make_shared<TextInputWidget>("");
    pw_input->setFixedSize(280.0f, 40.0f);

    // ── Status label (empty until auth attempted) ─────────────────────────────
    auto status_w = std::make_shared<TextWidget>("", 14.0f);
    status_w->setColor(Color{1.0f, 0.35f, 0.35f, 1.0f});
    status_w->setAlignment(Alignment::Center);

    // ── Unlock button ─────────────────────────────────────────────────────────
    // Actual unlock logic wired in on_init after xbus is available.
    auto unlock_btn_label = std::make_shared<TextWidget>("Unlock", 15.0f);
    auto unlock_btn = std::make_shared<ButtonWidget>(unlock_btn_label, [](){});

    // ── Layout ────────────────────────────────────────────────────────────────
    auto center_col = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{
            clock_w,
            std::make_shared<Spacer>(0, 4),
            date_w,
            std::make_shared<Spacer>(0, 48),
            user_w,
            std::make_shared<Spacer>(0, 12),
            pw_input,
            std::make_shared<Spacer>(0, 12),
            unlock_btn,
            std::make_shared<Spacer>(0, 8),
            status_w,
        });
    center_col->setGap(0.0f);
    center_col->setMainAxisAlignment(MainAxisAlignment::Center);
    center_col->setCrossAxisAlignment(CrossAxisAlignment::Center);

    // Background: dark fallback + wallpaper slot + scrim + auth panel
    auto wallpaper_bg = std::make_shared<ContainerWidget>(nullptr);
    wallpaper_bg->setBackgroundColor(Color{0.04f, 0.04f, 0.06f, 1.0f});
    wallpaper_bg->setFixedSize(10000.0f, 10000.0f);

    auto wallpaper_slot = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{});
    wallpaper_slot->setFixedSize(10000.0f, 10000.0f);

    // Semi-transparent dark scrim — improves clock/password readability over bright wallpapers
    auto scrim = std::make_shared<ContainerWidget>(nullptr);
    scrim->setBackgroundColor(Color{0.0f, 0.0f, 0.0f, 0.45f});
    scrim->setFixedSize(10000.0f, 10000.0f);

    auto auth_panel = std::make_shared<ContainerWidget>(center_col);

    auto root = std::make_shared<StackWidget>(std::vector<std::shared_ptr<Widget>>{
        wallpaper_bg, wallpaper_slot, scrim, auth_panel});
    root->setAlignment(Alignment::Center);

    return XApplication::Run(root, "/tmp/freebsdx.sock",
                              /*sandboxed=*/false, /*is_shell=*/true,
        [clock_w, date_w, pw_input, status_w, unlock_btn,
         wallpaper_slot](XBusClient& xbus) {
            XApplication::setTitle("Lock Screen");

            // Load wallpaper from server config
            xbus.Call("com.freebsdx.Wallpaper",
                static_cast<uint32_t>(WallpaperOpcode::GetWallpaper), {},
                [wallpaper_slot](const XBusMessage& resp) {
                    WallpaperSetRequest r{};
                    std::memcpy(&r, resp.payload_, sizeof(r));
                    if (r.path_[0]) {
                        static const ImageFitMode kFitMap[] = {
                            ImageFitMode::Cover, ImageFitMode::Contain,
                            ImageFitMode::Stretch, ImageFitMode::Center
                        };
                        auto fit = kFitMap[r.fit_mode_ < 4 ? r.fit_mode_ : 0];
                        auto img = std::make_shared<ImageWidget>(std::string(r.path_));
                        img->setFixedSize(10000.0f, 10000.0f);
                        img->setFitMode(fit);
                        wallpaper_slot->addChild(img);
                    }
                });

            // Register with LockService — compositor starts exclusive routing
            {
                XBusMessage reg{};
                reg.magic_ = XBUS_MAGIC;
                std::strncpy(reg.service_name_, "com.freebsdx.Lock",
                             sizeof(reg.service_name_) - 1);
                reg.opcode_ = static_cast<uint32_t>(LockOpcode::Register);
                xbus.Call("com.freebsdx.Lock",
                          static_cast<uint32_t>(LockOpcode::Register),
                          {reinterpret_cast<const uint8_t*>(reg.payload_), 0},
                          nullptr);
            }

            // Clock update timer (~1 Hz via KeyPress fabrication is too heavy;
            // use a broadcast subscription polling trick: update on any incoming msg).
            // Simpler: subscribe to broadcast and update clock on each frame.
            xbus.OnBroadcast([clock_w, date_w](const XBusMessage& msg) {
                // Update clock on any broadcast (runs on each compositor tick)
                clock_w->setText(currentTime());
                date_w->setText(currentDate());
                (void)msg;
            });

            // Wire unlock button
            auto do_unlock = [&xbus, pw_input, status_w, unlock_btn]() {
                const std::string& pw = pw_input->text();
                if (pw.empty()) return;

                LockAuthRequest req{};
                std::strncpy(req.username_, currentUser(),
                             sizeof(req.username_) - 1);
                std::strncpy(req.password_, pw.c_str(),
                             sizeof(req.password_) - 1);

                xbus.Call("com.freebsdx.Lock",
                          static_cast<uint32_t>(LockOpcode::Unlock),
                          {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                          [status_w](const XBusMessage& resp) {
                              LockState st{};
                              std::memcpy(&st, resp.payload_, sizeof(st));
                              if (st.locked_ == 0) {
                                  // Authenticated — LockService kills us shortly
                                  std::exit(0);
                              } else {
                                  status_w->setText("Incorrect password");
                              }
                          });
            };

            // Rewire button now that we have xbus in scope
            *unlock_btn = ButtonWidget(unlock_btn->child(), [do_unlock]() {
                do_unlock();
            });
        }, /*transparent_bg=*/false, /*shell_surface_id=*/fbx::shared::kLockSurfaceId);
}
