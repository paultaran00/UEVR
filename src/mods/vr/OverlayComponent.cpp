#include <glm/gtx/intersect.hpp>
#include <imgui_internal.h>

#include "Framework.hpp"
#include "../VR.hpp"
#include "../utility/ImGui.hpp"

#include "OverlayComponent.hpp"

// E-Day menu pointer state (see PluginLoader set/get_mod_value "VR_MenuPointer*")
namespace eday_menu {
bool enabled{false};
bool click_right{false}, click_left{false};
bool hover{false};
bool was_down{false};
int active{0}; // 0 right, 1 left
float hit_dist[2]{-1.0f, -1.0f};
float hit_uv[2][2]{};
float beam[2][6]{}; // start xyz, end xyz (tracking space, meters)
unsigned long long moved_at{0}; // when the mouse was last moved (scripts keep the gamepad prompts)
// the menu panel and head of the last pointer update (tracking space), for the desktop view
bool panel_ok{false};
glm::vec3 panel_c{}, panel_ax{}, panel_ay{}, panel_n{}, panel_head{};
float panel_w{1.0f}, panel_h{1.0f};
bool ray_ok[2]{false, false}; // the side's ray this frame (tracked controller or dev ray)
bool dev_ray_on[2]{false, false}; // "VR_MenuPointerDev" (PluginLoader.cpp)
float dev_ray[2][6]{};
}

// The beams are OpenVR overlays too (thin quads), so they are drawn over the game's 3D scene like the menu itself
// (as world geometry they ended behind the menu scene's floor and walls)
static vr::VROverlayHandle_t eday_beam_ov[2]{vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid};
static vr::VROverlayHandle_t eday_dot_ov[2]{vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid};
static const float EDAY_BEAM_W = 0.004f;  // m
static const float EDAY_DOT_W = 0.014f;   // m
static void edayBeamOverlays() {
    if (eday_beam_ov[0] != vr::k_ulOverlayHandleInvalid) return;
    static uint8_t beam_px[4 * 64 * 4];
    static uint8_t dot_px[16 * 16 * 4];
    for (int i = 0; i < 4 * 64; ++i) { beam_px[i * 4] = 70; beam_px[i * 4 + 1] = 150; beam_px[i * 4 + 2] = 255; beam_px[i * 4 + 3] = 200; }
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
        const float dx = x - 7.5f, dy = y - 7.5f;
        const bool in = dx * dx + dy * dy <= 56.0f;
        uint8_t* q = &dot_px[(y * 16 + x) * 4];
        q[0] = 190; q[1] = 225; q[2] = 255; q[3] = in ? 255 : 0;
    }
    for (int i = 0; i < 2; ++i) {
        const std::string n = std::string{"EDayBeam"} + (i == 0 ? "R" : "L");
        if (vr::VROverlay()->CreateOverlay(n.c_str(), n.c_str(), &eday_beam_ov[i]) == vr::VROverlayError_None) {
            vr::VROverlay()->SetOverlayRaw(eday_beam_ov[i], beam_px, 4, 64, 4);
            vr::VROverlay()->SetOverlayWidthInMeters(eday_beam_ov[i], EDAY_BEAM_W);
            vr::VROverlay()->SetOverlaySortOrder(eday_beam_ov[i], 10);
        }
        const std::string d = std::string{"EDayDot"} + (i == 0 ? "R" : "L");
        if (vr::VROverlay()->CreateOverlay(d.c_str(), d.c_str(), &eday_dot_ov[i]) == vr::VROverlayError_None) {
            vr::VROverlay()->SetOverlayRaw(eday_dot_ov[i], dot_px, 16, 16, 4);
            vr::VROverlay()->SetOverlayWidthInMeters(eday_dot_ov[i], EDAY_DOT_W);
            vr::VROverlay()->SetOverlaySortOrder(eday_dot_ov[i], 11);
        }
    }
}
// Show/Hide only on a change (called every frame before: suspected of compositor stutter)
static bool eday_shown[4]{false, false, false, false}; // (a new overlay starts hidden)
static void edayShow(int k, vr::VROverlayHandle_t h, bool on) {
    if (h == vr::k_ulOverlayHandleInvalid || eday_shown[k] == on) return;
    eday_shown[k] = on;
    if (on) vr::VROverlay()->ShowOverlay(h); else vr::VROverlay()->HideOverlay(h);
}
static void edayPlace(vr::VROverlayHandle_t h, const glm::vec3& xax, const glm::vec3& yax, const glm::vec3& zax, const glm::vec3& pos) {
    glm::mat4 m{1.0f};
    m[0] = glm::vec4{xax, 0.0f}; m[1] = glm::vec4{yax, 0.0f}; m[2] = glm::vec4{zax, 0.0f}; m[3] = glm::vec4{pos, 1.0f};
    const auto sm = Matrix3x4f{glm::rowMajor4(m)};
    vr::VROverlay()->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&sm);
}

bool eday_menu::desktop_pointer(float W, float H, DesktopPointer& out) {
    out = DesktopPointer{};
    if (!enabled || !panel_ok || W < 1.0f || H < 1.0f) return false;
    const auto& vr = VR::get();
    if (vr == nullptr || !vr->is_hmd_active()) return false;
    // the menu's normal pointing away from the head; depth = distance in front of the head along it
    const glm::vec3 nf = glm::dot(panel_c - panel_head, panel_n) >= 0.0f ? panel_n : -panel_n;
    const float D = glm::dot(panel_c - panel_head, nf);
    if (D < 0.05f) return false;
    const float NEAR_M = 0.1f;
    auto depth = [&](const glm::vec3& p) { return glm::dot(p - panel_head, nf); };
    auto to_px = [&](const glm::vec3& p, float* px) {
        const glm::vec3 q = panel_head + (p - panel_head) * (D / depth(p));
        px[0] = (glm::dot(q - panel_c, panel_ax) / panel_w + 0.5f) * W;
        px[1] = (0.5f - glm::dot(q - panel_c, panel_ay) / panel_h) * H;
    };
    const float px_per_m = H / panel_h;
    out.beam_px = std::max(3.0f, H / 360.0f);
    out.dot_px = std::max(12.0f, EDAY_DOT_W * px_per_m);
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        if (!ray_ok[i]) continue;
        glm::vec3 a{beam[i][0], beam[i][1], beam[i][2]}, b{beam[i][3], beam[i][4], beam[i][5]};
        float da = depth(a), db = depth(b);
        if (da < NEAR_M && db < NEAR_M) continue;
        // (the part behind the head is cut off)
        if (da < NEAR_M) { a = a + (b - a) * ((NEAR_M - da) / (db - da)); }
        if (db < NEAR_M) { b = b + (a - b) * ((NEAR_M - db) / (da - db)); }
        to_px(a, out.a[i]);
        to_px(b, out.b[i]);
        out.beam[i] = true;
        out.dot[i] = hit_dist[i] >= 0.0f;
        if (out.dot[i]) {
            // the dot exactly on the hit (the spot the mouse clicks)
            out.b[i][0] = hit_uv[i][0] * W;
            out.b[i][1] = hit_uv[i][1] * H;
        }
        any = true;
    }
    return any;
}

// openvr_draw: draw the beams/dots as OpenVR overlays here; on OpenXR only the hits, beam ends and clicks are worked
// out (the layers come from OverlayComponent::OpenXR::append_eday_pointer_layers)
static void edayMenuPointer(VR* vr, const glm::mat4& panel, float width_m, float height_m, bool openvr_draw = true) {
    using namespace eday_menu;
    hit_dist[0] = hit_dist[1] = -1.0f;
    if (!enabled) {
        // the beam overlays exist only while the pointer is on (title stuttered with them merely created)
        for (int i = 0; i < 2 && openvr_draw; ++i) {
            if (eday_beam_ov[i] != vr::k_ulOverlayHandleInvalid) { vr::VROverlay()->DestroyOverlay(eday_beam_ov[i]); eday_beam_ov[i] = vr::k_ulOverlayHandleInvalid; }
            if (eday_dot_ov[i] != vr::k_ulOverlayHandleInvalid) { vr::VROverlay()->DestroyOverlay(eday_dot_ov[i]); eday_dot_ov[i] = vr::k_ulOverlayHandleInvalid; }
        }
        for (int k = 0; k < 4; ++k) eday_shown[k] = false;
        ray_ok[0] = ray_ok[1] = false;
        if (was_down) {
            INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &in, sizeof(INPUT));
            was_down = false;
        }
        return;
    }
    if (openvr_draw) edayBeamOverlays();
    const glm::vec3 c{panel[3]};
    const glm::vec3 ax{glm::normalize(glm::vec3{panel[0]})};
    const glm::vec3 ay{glm::normalize(glm::vec3{panel[1]})};
    const glm::vec3 n{glm::normalize(glm::vec3{panel[2]})};
    const int idx[2]{vr->get_right_controller_index(), vr->get_left_controller_index()};
    panel_c = c; panel_ax = ax; panel_ay = ay; panel_n = n;
    panel_w = width_m; panel_h = height_m;
    panel_head = glm::vec3{vr->get_position(0)};
    panel_ok = width_m > 0.0f && height_m > 0.0f;
    for (int i = 0; i < 2; ++i) {
        ray_ok[i] = false;
        glm::vec3 o{}, dir{};
        if (dev_ray_on[i]) {
            o = glm::vec3{dev_ray[i][0], dev_ray[i][1], dev_ray[i][2]};
            dir = glm::vec3{dev_ray[i][3], dev_ray[i][4], dev_ray[i][5]};
            if (glm::length(dir) < 1e-4f) continue;
            dir = glm::normalize(dir);
        } else {
            if (idx[i] < 0) continue;
            o = glm::vec3{vr->get_aim_position(idx[i])};
            const auto rot = vr->get_aim_rotation(idx[i]);
            dir = glm::normalize(-glm::vec3{rot[2]});
        }
        ray_ok[i] = true;
        for (int k = 0; k < 3; ++k) { beam[i][k] = o[k]; beam[i][3 + k] = o[k] + dir[k] * 1.5f; }
        const float dn = glm::dot(dir, n);
        if (std::abs(dn) < 1e-4f) continue;
        const float t = glm::dot(c - o, n) / dn;
        if (t <= 0.0f || t > 20.0f) continue;
        const glm::vec3 h = o + dir * t;
        const float u = glm::dot(h - c, ax) / width_m + 0.5f;
        const float v = 0.5f - glm::dot(h - c, ay) / height_m;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
        hit_dist[i] = t;
        for (int k = 0; k < 3; ++k) beam[i][3 + k] = h[k];
        hit_uv[i][0] = u; hit_uv[i][1] = v;
    }
    // draw: each beam a quad from the controller to its end (hit or 1.5 m), turned to face the head; dots on hits
    const glm::vec3 head{vr->get_position(0)};
    for (int i = 0; i < 2 && openvr_draw; ++i) {
        if (eday_beam_ov[i] == vr::k_ulOverlayHandleInvalid || !ray_ok[i]) continue;
        const glm::vec3 a{beam[i][0], beam[i][1], beam[i][2]}, b{beam[i][3], beam[i][4], beam[i][5]};
        const glm::vec3 d = b - a;
        const float len = glm::length(d);
        if (len < 0.01f) { edayShow(i, eday_beam_ov[i], false); continue; }
        const glm::vec3 yax = d / len;
        const glm::vec3 mid = (a + b) * 0.5f;
        glm::vec3 to_eye = head - mid;
        glm::vec3 zax = to_eye - yax * glm::dot(to_eye, yax);
        if (glm::length(zax) < 1e-4f) zax = glm::vec3{0, 1, 0};
        zax = glm::normalize(zax);
        const glm::vec3 xax = glm::normalize(glm::cross(yax, zax));
        // the 4x64 texture is stretched along y: its height = width * 64/4 / texel aspect
        vr::VROverlay()->SetOverlayTexelAspect(eday_beam_ov[i], (64.0f / 4.0f) * EDAY_BEAM_W / len);
        edayPlace(eday_beam_ov[i], xax, yax, zax, mid);
        edayShow(i, eday_beam_ov[i], true);
        if (hit_dist[i] >= 0.0f) {
            edayPlace(eday_dot_ov[i], ax, ay, n, b + n * 0.002f);
            edayShow(2 + i, eday_dot_ov[i], true);
        } else {
            edayShow(2 + i, eday_dot_ov[i], false);
        }
    }
    if (click_left && hit_dist[1] >= 0.0f && !click_right) active = 1;
    if (click_right && hit_dist[0] >= 0.0f) active = 0;
    if (hit_dist[active] < 0.0f && hit_dist[1 - active] >= 0.0f) active = 1 - active;
    if (hit_dist[active] < 0.0f) return;

    const auto wnd = g_framework->get_window();
    if (wnd == nullptr || GetForegroundWindow() != wnd) return;
    RECT rc{};
    GetClientRect(wnd, &rc);
    POINT pt{(LONG)(hit_uv[active][0] * (rc.right - rc.left)), (LONG)(hit_uv[active][1] * (rc.bottom - rc.top))};
    ClientToScreen(wnd, &pt);
    // the mouse is used only for the click itself (moved there right before it): a mouse moving all the time made
    // the game switch its button prompts to the keyboard's and back, and the menus hitched with each switch
    // the mouse follows the pointer (menu items light up under it), but only when it moved a few pixels: a mouse
    // moved all the time made the game switch its button prompts to the keyboard's (scripts nudge it back)
    const bool down = active == 0 ? click_right : click_left;
    static POINT last{-10000, -10000};
    const bool moved = hover && (std::abs(pt.x - last.x) > 3 || std::abs(pt.y - last.y) > 3);
    if (moved || (down && !was_down)) {
        g_framework->remove_set_cursor_pos_patch();
        SetCursorPos(pt.x, pt.y);
        last = pt;
        eday_menu::moved_at = GetTickCount64();
    }
    if (down != was_down) {
        INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof(INPUT));
        was_down = down;
    }
}

namespace vrmod {
void OverlayComponent::on_reset() {
    m_overlay_data = {};
}

std::optional<std::string> OverlayComponent::on_initialize_openvr() {
    m_overlay_data = {};

    auto create_overlay = [this](const std::string& name, float size, vr::VROverlayHandle_t& out) -> std::optional<std::string> {
        // create vr overlay
        auto overlay_error = vr::VROverlay()->CreateOverlay(name.c_str(), name.c_str(), &out);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to create overlay: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }

        // set overlay to visible
        vr::VROverlay()->ShowOverlay(out);

        overlay_error = vr::VROverlay()->SetOverlayWidthInMeters(out, size);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay width: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }

        // same thing as above but absolute instead
        // get absolute tracking pose of hmd with GetDeviceToAbsoluteTrackingPose
        // then get the matrix from that
        // then set it as the overlay transform
        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
        vr::VROverlay()->SetOverlayTransformAbsolute(out, vr::TrackingUniverseStanding, &pose.mDeviceToAbsoluteTracking);

        // set overlay flag to receive smooth scroll events
        overlay_error = vr::VROverlay()->SetOverlayFlag(out, vr::VROverlayFlags::VROverlayFlags_SendVRSmoothScrollEvents, true);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay flag: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }

        spdlog::info("Made overlay with handle {}", out);
        return std::nullopt;
    };

    if (!create_overlay("REFramework", 0.25f, m_overlay_handle)) {
        auto overlay_error = vr::VROverlay()->SetOverlayInputMethod(m_overlay_handle, vr::VROverlayInputMethod_Mouse);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay input method: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }
    }

    if (!create_overlay("Slate", 2.0f, m_slate_overlay_handle)) {
        auto overlay_error = vr::VROverlay()->SetOverlayInputMethod(m_slate_overlay_handle, vr::VROverlayInputMethod_None);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay input method: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }
    }

    return std::nullopt;
}

void OverlayComponent::on_pre_imgui_frame() {
    this->update_input_openvr();
    this->update_input_mouse_emulation();
}

void OverlayComponent::update_input_mouse_emulation() {
    if (m_forced_aim) {
        VR::get()->set_aim_allowed(true);
        m_forced_aim = false;
    }

    if (VR::get()->get_runtime()->is_openvr() && m_framework_wrist_ui->value()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_mouse_move_time).count();
    const auto delta_f = std::min<float>(delta / 1000.0f, 0.5f);
    m_last_mouse_move_time = now;

    if (!m_framework_mouse_emulation->value()) {
        return;
    }

    if (m_framework_intersect_state.intersecting && VR::get()->is_using_controllers()) {
        auto vr = VR::get();
        auto& io = ImGui::GetIO();
        const auto is_initial_frame = !vr->is_using_afr() || vr->get_frame_count() % 2 == vr->m_left_eye_interval;

        const auto x = m_framework_intersect_state.swapchain_intersection_point.x;
        const auto y = m_framework_intersect_state.swapchain_intersection_point.y;
        
        const auto window_size = g_framework->get_last_window_size();
        const auto window_pos = g_framework->get_last_window_pos();

        // lerp towards the intersection point
        m_last_mouse_pos = glm::lerp(m_last_mouse_pos, glm::vec2{x, y}, delta_f * 10.0f);

        if (imgui::is_point_intersecting_any(m_last_mouse_pos.x, m_last_mouse_pos.y)) {
            io.MousePos = ImVec2{
                m_last_mouse_pos.x,
                m_last_mouse_pos.y
            };

            static bool was_pressing = false;
            static bool was_pressing_right = false;

            // Left click.
            const auto& a_button_right = !vr->m_swap_controllers->value() ? VR::s_action_a_button_right : VR::s_action_a_button_left;
            if (VR::get()->is_action_active_any_joystick(vr->get_action_handle(a_button_right))) {
                // Clear any gamepad A events.
                auto ctx = io.Ctx;

                if (ctx != nullptr) {
                    for (auto i = 0; i < ctx->InputEventsQueue.size(); ++i) {
                        auto& event = ctx->InputEventsQueue[i];

                        if (event.Type == ImGuiInputEventType::ImGuiInputEventType_Key && event.Key.Key == ImGuiKey_GamepadFaceDown) {
                            ctx->InputEventsQueue.erase(ctx->InputEventsQueue.begin() + i);
                            --i;
                        }
                    }
                }

                io.AddMouseButtonEvent(0, true);
                was_pressing = true;
            } else if (was_pressing) {
                io.AddMouseButtonEvent(0, false);
                was_pressing = false;
            }

            // Right click.
            const auto& b_button_right = !vr->m_swap_controllers->value() ? VR::s_action_b_button_right : VR::s_action_b_button_left;
            if (VR::get()->is_action_active_any_joystick(vr->get_action_handle(b_button_right))) {
                // Clear any gamepad B events.
                auto ctx = io.Ctx;

                if (ctx != nullptr) {
                    for (auto i = 0; i < ctx->InputEventsQueue.size(); ++i) {
                        auto& event = ctx->InputEventsQueue[i];

                        if (event.Type == ImGuiInputEventType::ImGuiInputEventType_Key && event.Key.Key == ImGuiKey_GamepadFaceLeft) {
                            ctx->InputEventsQueue.erase(ctx->InputEventsQueue.begin() + i);
                            --i;
                        }
                    }
                }

                io.AddMouseButtonEvent(1, true);
                was_pressing_right = true;
            } else if (was_pressing_right) {
                io.AddMouseButtonEvent(1, false);
                was_pressing_right = false;
            }

            const auto right_stick_axis = vr->get_right_stick_axis();
            
            // Mousewheel
            if (right_stick_axis.y > 0.5f) {
                io.MouseWheel += right_stick_axis.y * delta_f * 10.0f;
            } else if (right_stick_axis.y < -0.5f) {
                io.MouseWheel += right_stick_axis.y * delta_f * 10.0f;
            }

            VR::get()->set_aim_allowed(false);
            m_forced_aim = true;
        } else if (m_forced_aim) {
            VR::get()->set_aim_allowed(true);
            m_forced_aim = false;
        }
    }
}

void OverlayComponent::on_post_compositor_submit() {
    this->update_overlay_openvr();
    this->update_slate_openvr();
}


void OverlayComponent::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

void OverlayComponent::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
}

void OverlayComponent::on_draw_ui() {
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Overlay Options")) {
        if (VR::get()->get_runtime()->is_cylinder_layer_allowed()) {
            m_slate_overlay_type->draw("Overlay Type");

            if ((OverlayType)m_slate_overlay_type->value() == OverlayType::CYLINDER)  {
                m_slate_cylinder_angle->draw("UI Cylinder Angle");
            }
        }

        float ui_offset[] { m_slate_x_offset->value(), m_slate_y_offset->value(), m_slate_distance->value() };

        if (ImGui::SliderFloat3("UI Offset", ui_offset, -10.0f, 10.0f)) {
            m_slate_x_offset->value() = ui_offset[0];
            m_slate_y_offset->value() = ui_offset[1];
            m_slate_distance->value() = ui_offset[2];
        }

        m_slate_distance->draw("UI Distance");
        m_slate_size->draw("UI Size");
        m_ui_follows_view->draw("UI Follows View");
        ImGui::SameLine();
        m_ui_invert_alpha->draw("UI Invert Alpha");

        m_framework_distance->draw("Framework Distance");
        m_framework_size->draw("Framework Size");
        m_framework_ui_follows_view->draw("Framework Follows View");
        if (VR::get()->get_runtime()->is_openvr()) {
            ImGui::SameLine();
            m_framework_wrist_ui->draw("Framework Wrist UI");
        }
        m_framework_mouse_emulation->draw("Framework Mouse Emulation");
        ImGui::TreePop();
    }
}

void OverlayComponent::update_input_openvr() {
    if (!VR::get()->get_runtime()->is_openvr()) {
        return;
    }

    if (!m_framework_wrist_ui->value()) {
        return;
    }

    auto vr = VR::get();
    auto& io = ImGui::GetIO();
    const auto is_initial_frame = !vr->is_using_afr() || vr->get_frame_count() % 2 == vr->m_left_eye_interval;

    // Restore the previous frame's input state
    memcpy(io.KeysDown, m_initial_imgui_input_state.KeysDown, sizeof(io.KeysDown));
    memcpy(io.MouseDown, m_initial_imgui_input_state.MouseDown, sizeof(io.MouseDown));
    io.MousePos = m_initial_imgui_input_state.MousePos;
    io.MouseWheel = m_initial_imgui_input_state.MouseWheel;
    io.MouseWheelH = m_initial_imgui_input_state.MouseWheelH;
    io.KeyCtrl = m_initial_imgui_input_state.KeyCtrl;
    io.KeyShift = m_initial_imgui_input_state.KeyShift;
    io.KeyAlt = m_initial_imgui_input_state.KeyAlt;
    io.KeySuper = m_initial_imgui_input_state.KeySuper;

    if (!is_initial_frame) {
        return;
    }

    io.MouseWheel = 0;
    io.MouseWheelH = 0;

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    const auto last_window_pos = g_framework->get_last_window_pos();
    const auto last_window_size = g_framework->get_last_window_size();
    const auto rendertarget_width = is_d3d11 ? g_framework->get_rendertarget_width_d3d11() : g_framework->get_rendertarget_width_d3d12();
    const auto rendertarget_height = is_d3d11 ? g_framework->get_rendertarget_height_d3d11() : g_framework->get_rendertarget_height_d3d12();

    // Poll overlay events
    vr::VREvent_t event{};
    const auto hwnd = g_framework->get_window();

    while (vr::VROverlay()->PollNextOverlayEvent(m_overlay_handle, &event, sizeof(event))) {
        switch (event.eventType) {
            case vr::VREvent_MouseButtonDown:
                m_initial_imgui_input_state.MouseDown[0] = true;
                io.MouseDown[0] = true;
                break;
            case vr::VREvent_MouseButtonUp:
                m_initial_imgui_input_state.MouseDown[0] = false;
                io.MouseDown[0] = false;
                break;
            case vr::VREvent_MouseMove: {
                const std::array<float, 2> raw_coords { event.data.mouse.x, event.data.mouse.y };

                // Convert from GL space (bottom left is 0,0) to window space (top left is 0,0)
                const auto mouse_point = ImVec2{
                    raw_coords[0],
                    (rendertarget_height - raw_coords[1])
                };

                // override imgui mouse position
                m_initial_imgui_input_state.MousePos = mouse_point;
                io.MousePos = mouse_point;
            } break;
            case vr::VREvent_ScrollSmooth: {
                m_initial_imgui_input_state.MouseWheelH += event.data.scroll.xdelta;
                m_initial_imgui_input_state.MouseWheel += event.data.scroll.ydelta;
                io.MouseWheelH = event.data.scroll.xdelta;
                io.MouseWheel = event.data.scroll.ydelta;
            } break;
            default:
                break;
        }
    }
}

void OverlayComponent::update_slate_openvr() {
    auto vr = VR::get();

    if (!vr->get_runtime()->is_openvr()) {
        return;
    }

    if (!vr->is_gui_enabled()) {
        vr::VROverlay()->ClearOverlayTexture(m_slate_overlay_handle);
        return;
    }

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    // TODO: do the sizing / scaling calculations below need to take into account non-standard VRTextureBounds_t
    // when we force a symmetrical eye projection matrix?
    vr::VRTextureBounds_t bounds{};
    bounds.uMin = 0.0f;
    bounds.uMax = 1.0f;
    bounds.vMin = 0.0f;
    bounds.vMax = 1.0f;

    vr::VROverlay()->SetOverlayTextureBounds(m_slate_overlay_handle, &bounds);

    vr::TrackedDevicePose_t pose{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);

    auto rotation_offset = glm::inverse(vr->get_rotation_offset());

    if (vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
        const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
        const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

        // Add the inverse of the pitch rotation to the rotation offset
        rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
    }

    //auto glm_matrix = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
    auto glm_matrix = Matrix4x4f{rotation_offset};
    if (m_ui_follows_view->value()) {
        const auto mat = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
        glm_matrix = glm::extractMatrixRotation(mat);
        glm_matrix[3] += mat[3];
    } else {
        glm_matrix[3] += vr->get_standing_origin();
    }

    glm_matrix[3] -= glm_matrix[2] * m_slate_distance->value();
    glm_matrix[3] += m_slate_x_offset->value() * glm_matrix[0];
    glm_matrix[3] += m_slate_y_offset->value() * glm_matrix[1];
    glm_matrix[3].w = 1.0f;
    
    const auto steamvr_matrix = Matrix3x4f{glm::rowMajor4(glm_matrix)};
    vr::VROverlay()->SetOverlayTransformAbsolute(m_slate_overlay_handle, vr::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&steamvr_matrix);

    const auto is_d3d12 = g_framework->get_renderer_type() == Framework::RendererType::D3D12;
    const auto size = is_d3d12 ? g_framework->get_d3d12_rt_size() : g_framework->get_d3d11_rt_size();
    const auto aspect = size.x / size.y;
    const auto width_meters = m_slate_size->value() * aspect;
    vr::VROverlay()->SetOverlayWidthInMeters(m_slate_overlay_handle, width_meters);

    // E-Day: menu pointer - each controller's aim ray against the UI panel; scripts read the hit distances
    // ("VR_MenuPointerHit") to draw the beams, and the right one (or the last one clicking) moves the mouse.
    edayMenuPointer(vr.get(), glm_matrix, width_meters, m_slate_size->value());

    if (is_d3d11) {
        if (vr->m_d3d11.get_ui_tex().Get() == nullptr) {
            return;
        }

        vr::Texture_t ui_tex{(void*)vr->m_d3d11.get_ui_tex().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
        vr::VROverlay()->SetOverlayTexture(m_slate_overlay_handle, &ui_tex);   
    } else {
        if (vr->m_d3d12.get_openvr_ui_tex().texture.Get() == nullptr) {
            return;
        }

        vr::D3D12TextureData_t overlay_tex {
            vr->m_d3d12.get_openvr_ui_tex().texture.Get(),
            g_framework->get_d3d12_hook()->get_command_queue(),
            0
        };

        vr::Texture_t ui_tex{(void*)&overlay_tex, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
        const auto err = vr::VROverlay()->SetOverlayTexture(m_slate_overlay_handle, &ui_tex);

        static auto last_err = vr::VROverlayError_None;
        static auto last_log = std::chrono::steady_clock::time_point{};
        if (err != last_err || std::chrono::steady_clock::now() - last_log > std::chrono::seconds(10)) {
            last_log = std::chrono::steady_clock::now();
            spdlog::info("[VR] Slate overlay SetOverlayTexture -> {} (visible: {})", vr::VROverlay()->GetOverlayErrorNameFromEnum(err),
                vr::VROverlay()->IsOverlayVisible(m_slate_overlay_handle));
            last_err = err;
        }
    }
}

bool OverlayComponent::update_wrist_overlay_openvr() {
    if (!VR::get()->get_runtime()->is_openvr()) {
        return false;
    }

    auto& vr = VR::get();

    if (!m_framework_wrist_ui->value()) {
        return true;
    }

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    // update REFramework menu overlay
    const auto last_window_pos = g_framework->get_last_window_pos();
    const auto last_window_size = g_framework->get_last_window_size();
    const auto render_target_width = is_d3d11 ? g_framework->get_rendertarget_width_d3d11() : g_framework->get_rendertarget_width_d3d12();
    const auto render_target_height = is_d3d11 ? g_framework->get_rendertarget_height_d3d11() : g_framework->get_rendertarget_height_d3d12();

    // only update certain parts of the overlay
    // if things like the width or position of the window change
    if (m_overlay_data.last_x != last_window_pos.x || m_overlay_data.last_y != last_window_pos.y ||
        m_overlay_data.last_width != last_window_size.x || m_overlay_data.last_height != last_window_size.y ||
        m_overlay_data.last_render_target_width != render_target_width || m_overlay_data.last_render_target_height != render_target_height || m_just_closed_ui || m_just_opened_ui) 
    {
        // scaling for the intersection mask
        // so it doesn't become too intrusive during gameplay
        const auto scale = m_closed_ui ? 0.25f : 1.0f;

        // TODO: do the sizing / scaling calculations below need to take into account non-standard VRTextureBounds_t
        // when we force a symmetrical eye projection matrix?
        vr::VRTextureBounds_t bounds{};
        bounds.uMin = last_window_pos.x / render_target_width ;
        bounds.uMax = (last_window_pos.x + last_window_size.x) / render_target_width;
        bounds.vMin = last_window_pos.y / render_target_height;
        bounds.vMax = (last_window_pos.y + last_window_size.y) / render_target_height;

        vr::VROverlay()->SetOverlayTextureBounds(m_overlay_handle, &bounds);

        // necessary, fixes all sorts of issues with ray intersection
        const auto mouse_scale = vr::HmdVector2_t{(float)render_target_width, (float)render_target_height};
        vr::VROverlay()->SetOverlayMouseScale(m_overlay_handle, &mouse_scale);

        vr::VROverlayIntersectionMaskPrimitive_t intersection_mask{};

        intersection_mask.m_nPrimitiveType = vr::EVROverlayIntersectionMaskPrimitiveType::OverlayIntersectionPrimitiveType_Rectangle;
        intersection_mask.m_Primitive.m_Rectangle.m_flTopLeftX = last_window_pos.x;
        intersection_mask.m_Primitive.m_Rectangle.m_flTopLeftY = last_window_pos.y;
        intersection_mask.m_Primitive.m_Rectangle.m_flWidth = last_window_size.x;
        intersection_mask.m_Primitive.m_Rectangle.m_flHeight = last_window_size.y;

        vr::VROverlay()->SetOverlayIntersectionMask(m_overlay_handle, &intersection_mask, 1);

        // and now set the last known values
        m_overlay_data.last_x = last_window_pos.x;
        m_overlay_data.last_y = last_window_pos.y;
        m_overlay_data.last_width = last_window_size.x;
        m_overlay_data.last_height = last_window_size.y;
        m_overlay_data.last_render_target_width = render_target_width;
        m_overlay_data.last_render_target_height = render_target_height;
    }

    // Fire an intersection test and enable the laser pointer if we're intersecting
    const auto& controllers = vr->get_controllers();

    bool should_show_overlay = !m_closed_ui;

    if (controllers.size() >= 2 && !vr->is_any_action_down()) {
        Matrix4x4f left_controller_world_transform{glm::identity<Matrix4x4f>()};

        // Attach the overlay to the left controller
        if (controllers[0] != vr::k_unTrackedDeviceIndexInvalid) {
            const auto position_offset = vr->m_overlay_position;
            const auto rotation_offset = vr->m_overlay_rotation;
 
            left_controller_world_transform = vr->get_transform(controllers[0]) * Matrix4x4f{glm::quat{rotation_offset}};
            left_controller_world_transform[3] -= glm::extractMatrixRotation(left_controller_world_transform) * position_offset;
            left_controller_world_transform[3].w = 1.0f;

            const auto steamvr_transform = Matrix3x4f{ glm::rowMajor4(left_controller_world_transform) };
            
            vr::VROverlay()->SetOverlayTransformAbsolute(m_overlay_handle, vr::ETrackingUniverseOrigin::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&steamvr_transform);
        }

        bool any_intersected = false;

        for (const auto& controller_index : controllers) {
            const auto is_left = controller_index == controllers[0];

            vr::VRInputValueHandle_t controller_handle{};

            if (is_left) {
                controller_handle = vr->m_left_joystick;
            } else {
                controller_handle = vr->m_right_joystick;
            }

            char render_name[vr::k_unMaxPropertyStringSize]{};
            vr::VRSystem()->GetStringTrackedDeviceProperty(controller_index, vr::Prop_RenderModelName_String, render_name, vr::k_unMaxPropertyStringSize);

            vr::RenderModel_ControllerMode_State_t controller_state{};
            vr::RenderModel_ComponentState_t component_state{};

            // get tip component state
            if (!vr::VRRenderModels()->GetComponentStateForDevicePath(render_name, vr::k_pch_Controller_Component_Tip, controller_handle, &controller_state, &component_state)) {
                continue;
            }

            // obtain tip world transform
            const auto controller_world_transform = vr->get_transform(controller_index);
            const auto tip_local_transform = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&component_state.mTrackingToComponentLocal});
            const auto tip_world_transform = controller_world_transform * tip_local_transform;

            // Set up intersection data
            vr::VROverlayIntersectionResults_t intersection_results{};
            vr::VROverlayIntersectionParams_t intersection_params{};
            intersection_params.eOrigin = vr::TrackingUniverseOrigin::TrackingUniverseStanding;
            intersection_params.vSource.v[0] = tip_world_transform[3][0];
            intersection_params.vSource.v[1] = tip_world_transform[3][1];
            intersection_params.vSource.v[2] = tip_world_transform[3][2];
            intersection_params.vDirection.v[0] = -tip_world_transform[2][0];
            intersection_params.vDirection.v[1] = -tip_world_transform[2][1];
            intersection_params.vDirection.v[2] = -tip_world_transform[2][2];

            // Do the intersection test
            if (vr::VROverlay()->ComputeOverlayIntersection(m_overlay_handle, &intersection_params, &intersection_results)) {
                auto normal = Vector4f{intersection_results.vNormal.v[0], intersection_results.vNormal.v[1], intersection_results.vNormal.v[2], 1.0f};
                normal = glm::inverse(glm::extractMatrixRotation(tip_world_transform)) * normal;

                if (m_closed_ui) {
                    const auto u = ((intersection_results.vUVs.v[0] * m_overlay_data.last_render_target_width) - m_overlay_data.last_x) / m_overlay_data.last_width;
                    const auto v = ((m_overlay_data.last_render_target_height - (intersection_results.vUVs.v[1] * m_overlay_data.last_render_target_height)) - m_overlay_data.last_y) / m_overlay_data.last_height;

                    any_intersected = u >= 0.25f &&
                                    u <= 0.75f && 
                                    v >= 0.25f && 
                                    v <= 0.75f;

                    // Make sure the intersection hit the front of the overlay, not the back
                    any_intersected = any_intersected && normal.z > 0.0f;
                } else {
                    any_intersected = normal.z > 0.0f;
                }
            }
        }

        // Do the same intersection test with the user's view/head gaze
        // Both the head and controller needs to be aimed at the overlay for it to appear
        if (any_intersected) {
            auto head_world_transform = vr->get_transform(vr::k_unTrackedDeviceIndex_Hmd);
            vr::VROverlayIntersectionResults_t intersection_results{};
            vr::VROverlayIntersectionParams_t intersection_params{};
            intersection_params.eOrigin = vr::TrackingUniverseOrigin::TrackingUniverseStanding;
            intersection_params.vSource.v[0] = head_world_transform[3][0];
            intersection_params.vSource.v[1] = head_world_transform[3][1];
            intersection_params.vSource.v[2] = head_world_transform[3][2];
            intersection_params.vDirection.v[0] = -head_world_transform[2][0];
            intersection_params.vDirection.v[1] = -head_world_transform[2][1];
            intersection_params.vDirection.v[2] = -head_world_transform[2][2];

            if (vr::VROverlay()->ComputeOverlayIntersection(m_overlay_handle, &intersection_params, &intersection_results)) {
                if (m_closed_ui) {
                    const auto u = ((intersection_results.vUVs.v[0] * m_overlay_data.last_render_target_width) - m_overlay_data.last_x) / m_overlay_data.last_width;
                    const auto v = ((m_overlay_data.last_render_target_height - (intersection_results.vUVs.v[1] * m_overlay_data.last_render_target_height)) - m_overlay_data.last_y) / m_overlay_data.last_height;

                    any_intersected = u >= 0.25f &&
                                    u <= 0.75f && 
                                    v >= 0.25f && 
                                    v <= 0.75f;
                }

                auto normal = Vector4f{intersection_results.vNormal.v[0], intersection_results.vNormal.v[1], intersection_results.vNormal.v[2], 1.0f};
                normal = glm::inverse(glm::extractMatrixRotation(head_world_transform)) * normal;

                // Make sure the intersection hit the front of the overlay, not the back
                any_intersected = any_intersected && normal.z > 0.0f;
            } else {
                any_intersected = false;
            }
        }

        // set overlay flag
        if (any_intersected) {
            should_show_overlay = true;
            vr::VROverlay()->SetOverlayFlag(m_overlay_handle, vr::VROverlayFlags::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

            g_framework->set_draw_ui(true);

            if (m_closed_ui) {
                m_just_opened_ui = true;
            } else {
                m_just_opened_ui = false;
            }

            m_closed_ui = false;
            m_just_closed_ui = false;
        } else {
            should_show_overlay = false;
            vr::VROverlay()->SetOverlayFlag(m_overlay_handle, vr::VROverlayFlags::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

            if (!m_closed_ui) {
                g_framework->set_draw_ui(false);

                m_closed_ui = true;
                m_just_closed_ui = true;
            } else {
                m_just_closed_ui = false;
            }

            m_just_opened_ui = false;
        }
    }

    return should_show_overlay;
}

void OverlayComponent::update_overlay_openvr() {
    if (!VR::get()->get_runtime()->is_openvr()) {
        return;
    }

    auto& vr = VR::get();

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    bool should_show_overlay = update_wrist_overlay_openvr();

    if (m_framework_wrist_ui->value()) {
        vr::VROverlay()->ShowOverlay(m_overlay_handle); // always show overlay idk look at it later

        if (should_show_overlay) {
            // finally set the texture
            if (is_d3d11) {
                vr::Texture_t imgui_tex{(void*)g_framework->get_rendertarget_d3d11().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);   
            } else {
                auto& hook = g_framework->get_d3d12_hook();

                vr::D3D12TextureData_t texture_data {
                    g_framework->get_rendertarget_d3d12().Get(),
                    hook->get_command_queue(),
                    0
                };
                
                vr::Texture_t imgui_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
            }
        } else {
            if (is_d3d11) {
                // draw a blank texture (don't just call HideOverlay, we'll no longer be able to use intersection tests)
                vr::Texture_t imgui_tex{(void*)g_framework->get_blank_rendertarget_d3d11().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
            } else {
                auto& hook = g_framework->get_d3d12_hook();

                vr::D3D12TextureData_t texture_data {
                    g_framework->get_blank_rendertarget_d3d12().Get(),
                    hook->get_command_queue(),
                    0
                };
                
                vr::Texture_t imgui_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
            }
        }

        return;
    }

    // Draw the UI as a plane in front of the user instead
    if (!m_framework_wrist_ui->value() && g_framework->is_drawing_anything()) {
        vr::VROverlay()->ShowOverlay(m_overlay_handle);

        // Show the entire texture
        // TODO: do the sizing / scaling calculations below need to take into account non-standard VRTextureBounds_t
        // when we force a symmetrical eye projection matrix?
        vr::VRTextureBounds_t bounds{};
        bounds.uMin = 0.0f;
        bounds.uMax = 1.0f;
        bounds.vMin = 0.0f;
        bounds.vMax = 1.0f;

        vr::VROverlay()->SetOverlayTextureBounds(m_overlay_handle, &bounds);

        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);

        //auto glm_matrix = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
        auto rotation_offset = glm::inverse(vr->get_rotation_offset());

        // If we're not drawing the UI, this means we want to draw the cursor all the time
        // So we need to rotate the UI's pitch as well
        if (!g_framework->is_drawing_ui() && vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
            const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
            const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

            // Add the inverse of the pitch rotation to the rotation offset
            rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
        }

        auto glm_matrix = Matrix4x4f{rotation_offset};

        if (m_framework_ui_follows_view->value()) {
            // todo
        }

        glm_matrix[3] += vr->get_standing_origin();

        if (g_framework->is_drawing_ui()) {
            glm_matrix[3] -= glm_matrix[2] * m_framework_distance->value();
        } else {
            glm_matrix[3] -= glm_matrix[2] * (m_slate_distance->value() - 0.01f);

            glm_matrix[3] += m_slate_x_offset->value() * glm_matrix[0];
            glm_matrix[3] += m_slate_y_offset->value() * glm_matrix[1];
        }

        glm_matrix[3].w = 1.0f;
        const auto steamvr_matrix = Matrix3x4f{glm::rowMajor4(glm_matrix)};
        vr::VROverlay()->SetOverlayTransformAbsolute(m_overlay_handle, vr::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&steamvr_matrix);

        const auto is_d3d12 = g_framework->get_renderer_type() == Framework::RendererType::D3D12;
        const auto size = is_d3d12 ? g_framework->get_d3d12_rt_size() : g_framework->get_d3d11_rt_size();
        const auto aspect = size.x / size.y;
        const auto size_meters = g_framework->is_drawing_ui() ? m_framework_size->value() : m_slate_size->value();

        const float scale_factor = g_framework->is_drawing_ui() ? (size.x / 1920.0f) : 1.0f;
        const float adjusted_size_meters = size_meters * scale_factor;

        const auto width_meters = adjusted_size_meters * aspect;
        const auto height_meters = adjusted_size_meters;

        vr::VROverlay()->SetOverlayWidthInMeters(m_overlay_handle, width_meters);

        if (is_d3d11) {
            vr::Texture_t imgui_tex{(void*)g_framework->get_rendertarget_d3d11().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
            vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);   
        } else {
            auto& hook = g_framework->get_d3d12_hook();

            vr::D3D12TextureData_t texture_data {
                g_framework->get_rendertarget_d3d12().Get(),
                hook->get_command_queue(),
                0
            };
            
            vr::Texture_t imgui_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
            vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
        }

        // Check if the controller pointer intersects with the quad, and we can use this to emulate the mouse
        if (vr->is_using_controllers()) {
            // Right only for now for testing
            const auto controller_index = !vr->m_swap_controllers->value() ? vr->get_right_controller_index() : vr->get_left_controller_index();
            const auto right_controller_rot = glm::quat{vr->get_rotation(controller_index, false)};
            const auto right_controller_pos = glm::vec3{vr->get_position(controller_index, false)};

            const auto start = right_controller_pos;
            auto fwd = (right_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f});
            const auto end = right_controller_pos + (fwd * 1000.0f);
            
            const auto plane_pos = glm::vec3{glm_matrix[3]};

            float intersection_distance = 0.0f;

            auto& intersect_state = g_framework->is_drawing_ui() ? m_framework_intersect_state : m_intersect_state;
            auto& other_intersect_state = g_framework->is_drawing_ui() ? m_intersect_state : m_framework_intersect_state;
            other_intersect_state.intersecting = false;
            if (glm::intersectRayPlane<glm::vec3>(start, fwd, plane_pos, glm::normalize(glm::vec3{glm_matrix[2]}), intersection_distance)) {
                const auto intersection_point = start + (fwd * intersection_distance);

                const auto local_point = glm::inverse(glm_matrix) * glm::vec4{intersection_point, 1.0f};

                const auto w_half = width_meters / 2.0f;
                const auto h_half = height_meters / 2.0f;

                if (local_point.x >= -w_half && local_point.x <= w_half && local_point.y >= -h_half && local_point.y <= h_half) {
                    const auto x = (local_point.x + w_half) / width_meters;
                    const auto y = (height_meters - (local_point.y + h_half)) / height_meters;

                    intersect_state.quad_intersection_point = {x, y};
                    intersect_state.intersecting = true;

                    /*if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI); it != vr->m_openxr->swapchains.end()) {
                        const auto client_x = (int32_t)((float)it->second.width * x);
                        const auto client_y = (int32_t)((float)it->second.height * y);

                        intersect_state.swapchain_intersection_point = {client_x, client_y};
                    }*/
                    // Change to openvr version
                    const auto client_x = (int32_t)(size.x * x);
                    const auto client_y = (int32_t)(size.y * y);

                    intersect_state.swapchain_intersection_point = {client_x, client_y};
                } else {
                    intersect_state.intersecting = false;
                }
            }
        } else {
            m_framework_intersect_state.intersecting = false;
            m_intersect_state.intersecting = false;
        }
    } else {
        vr::VROverlay()->ClearOverlayTexture(m_overlay_handle);
        vr::VROverlay()->HideOverlay(m_overlay_handle);
    }
}

std::optional<std::reference_wrapper<XrCompositionLayerQuad>> OverlayComponent::OpenXR::generate_slate_quad(
    runtimes::OpenXR::SwapchainIndex swapchain, 
    XrEyeVisibility eye) 
{
    auto& vr = VR::get();

    if (!vr->is_gui_enabled()) {
        m_parent->m_intersect_state.intersecting = false;
        return std::nullopt;
    }

    if (!vr->m_openxr->swapchains.contains((uint32_t)swapchain)) {
        return std::nullopt;
    }

    const auto is_left_eye = eye == XR_EYE_VISIBILITY_BOTH || eye == XR_EYE_VISIBILITY_LEFT;

    auto& layer = is_left_eye ? this->m_slate_layer : this->m_slate_layer_right;

    layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    const auto& ui_swapchain = vr->m_openxr->swapchains[(uint32_t)swapchain];
    layer.subImage.swapchain = ui_swapchain.handle;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.subImage.imageRect.offset.x = 0;
    layer.subImage.imageRect.offset.y = 0;
    layer.subImage.imageRect.extent.width = ui_swapchain.width;
    layer.subImage.imageRect.extent.height = ui_swapchain.height;
    layer.eyeVisibility = eye;

    auto glm_matrix = glm::identity<glm::mat4>();

    if (vr->m_overlay_component.m_ui_follows_view->value()) {
        layer.space = vr->m_openxr->view_space;
    } else {
        auto rotation_offset = glm::inverse(vr->get_rotation_offset());

        if (vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
            const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
            const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

            // Add the inverse of the pitch rotation to the rotation offset
            rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
        }

        glm_matrix = Matrix4x4f{rotation_offset};   
        glm_matrix[3] += vr->get_standing_origin();
        layer.space = vr->m_openxr->stage_space;
    }

    const auto size_meters = m_parent->m_slate_size->value();
    const auto meters_w = (float)ui_swapchain.width / (float)ui_swapchain.height * size_meters;
    const auto meters_h = size_meters;
    layer.size = {meters_w, meters_h};

    glm_matrix[3] -= glm_matrix[2] * m_parent->m_slate_distance->value();
    glm_matrix[3] += m_parent->m_slate_x_offset->value() * glm_matrix[0];
    glm_matrix[3] += m_parent->m_slate_y_offset->value() * glm_matrix[1];
    glm_matrix[3].w = 1.0f;

    layer.pose.orientation = runtimes::OpenXR::to_openxr(glm::quat_cast(glm_matrix));
    layer.pose.position = runtimes::OpenXR::to_openxr(glm_matrix[3]);

    // E-Day: menu pointer - each controller's aim ray against this panel (in stage space, as the aim poses), once a
    // frame; hits and clicks as on OpenVR, the beams/dots are drawn by append_eday_pointer_layers
    if (is_left_eye) {
        auto panel = glm_matrix;

        if (vr->m_overlay_component.m_ui_follows_view->value()) {
            panel = vr->get_transform(0) * glm_matrix; // (view space -> stage space)
        }

        edayMenuPointer(vr.get(), panel, meters_w, meters_h, false);
    }

    // Check if the controller pointer intersects with the quad, and we can use this to emulate the mouse
    if (vr->is_using_controllers()) {
        // Right only for now for testing
        const auto controller_index = !vr->m_swap_controllers->value() ? vr->get_right_controller_index() : vr->get_left_controller_index();
        const auto right_controller_rot = glm::quat{vr->get_rotation(controller_index, false)};
        const auto right_controller_pos = glm::vec3{vr->get_position(controller_index, false)};

        const auto start = right_controller_pos;
        auto fwd = (right_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f});
        const auto end = right_controller_pos + (fwd * 1000.0f);

        const auto plane_pos = glm::vec3{glm_matrix[3]};

        float intersection_distance = 0.0f;
        if (glm::intersectRayPlane<glm::vec3>(start, fwd, plane_pos, glm::normalize(glm::vec3{glm_matrix[2]}), intersection_distance)) {
            const auto intersection_point = start + (fwd * intersection_distance);

            const auto local_point = glm::inverse(glm_matrix) * glm::vec4{intersection_point, 1.0f};

            const auto w_half = meters_w / 2.0f;
            const auto h_half = meters_h / 2.0f;

            if (local_point.x >= -w_half && local_point.x <= w_half && local_point.y >= -h_half && local_point.y <= h_half) {
                const auto x = (local_point.x + w_half) / meters_w;
                const auto y = (meters_h - (local_point.y + h_half)) / meters_h;

                m_parent->m_intersect_state.quad_intersection_point = {x, y};
                m_parent->m_intersect_state.intersecting = true;

                if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI); it != vr->m_openxr->swapchains.end()) {
                    const auto client_x = (int32_t)((float)it->second.width * x);
                    const auto client_y = (int32_t)((float)it->second.height * y);

                    m_parent->m_intersect_state.swapchain_intersection_point = {client_x, client_y};
                }
            } else {
                m_parent->m_intersect_state.intersecting = false;
            }
        }
    } else {
        m_parent->m_intersect_state.intersecting = false;
    }

    return layer;
}

std::optional<std::reference_wrapper<XrCompositionLayerCylinderKHR>> OverlayComponent::OpenXR::generate_slate_cylinder(
    runtimes::OpenXR::SwapchainIndex swapchain, 
    XrEyeVisibility eye) 
{
    auto& vr = VR::get();

    if (!vr->is_gui_enabled()) {
        return std::nullopt;
    }

    if (!vr->m_openxr->swapchains.contains((uint32_t)swapchain)) {
        return std::nullopt;
    }

    const auto is_left_eye = eye == XR_EYE_VISIBILITY_BOTH || eye == XR_EYE_VISIBILITY_LEFT;

    auto& layer = is_left_eye ? this->m_slate_layer_cylinder : this->m_slate_layer_cylinder_right;

    layer.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
    const auto& ui_swapchain = vr->m_openxr->swapchains[(uint32_t)swapchain];
    layer.subImage.swapchain = ui_swapchain.handle;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.subImage.imageRect.offset.x = 0;
    layer.subImage.imageRect.offset.y = 0;
    layer.subImage.imageRect.extent.width = ui_swapchain.width;
    layer.subImage.imageRect.extent.height = ui_swapchain.height;
    layer.eyeVisibility = eye;
    
    auto glm_matrix = glm::identity<glm::mat4>();

    if (vr->m_overlay_component.m_ui_follows_view->value()) {
        layer.space = vr->m_openxr->view_space;
    } else {
        auto rotation_offset = glm::inverse(vr->get_rotation_offset());

        if (vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
            const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
            const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

            // Add the inverse of the pitch rotation to the rotation offset
            rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
        }

        glm_matrix = Matrix4x4f{rotation_offset};   
        glm_matrix[3] += vr->get_standing_origin();
        layer.space = vr->m_openxr->stage_space;
    }

    const auto size_meters = m_parent->m_slate_size->value();
    const auto meters_w = (float)ui_swapchain.width / (float)ui_swapchain.height * size_meters;
    const auto meters_h = size_meters;

    // OpenXR Docs:
    // radius is the non-negative radius of the cylinder. Values of zero or floating point positive infinity are treated as an infinite cylinder.
    // centralAngle is the angle of the visible section of the cylinder, based at 0 radians, in the range of [0, 2π). It grows symmetrically around the 0 radian angle.
    // aspectRatio is the ratio of the visible cylinder section width / height. The height of the cylinder is given by: (cylinder radius × cylinder angle) / aspectRatio.
    layer.centralAngle = glm::max<float>(1.0f, glm::radians(m_parent->m_slate_cylinder_angle->value()));
    layer.aspectRatio = (meters_w / meters_h);
    layer.radius = (meters_h / layer.centralAngle) * layer.aspectRatio;

    glm_matrix[3] -= glm_matrix[2] * m_parent->m_slate_distance->value();
    glm_matrix[3] += glm_matrix[2] * layer.radius;
    glm_matrix[3] += m_parent->m_slate_x_offset->value() * glm_matrix[0];
    glm_matrix[3] += m_parent->m_slate_y_offset->value() * glm_matrix[1];
    glm_matrix[3].w = 1.0f;

    layer.pose.orientation = runtimes::OpenXR::to_openxr(glm::quat_cast(glm_matrix));
    layer.pose.position = runtimes::OpenXR::to_openxr(glm_matrix[3]);

    return layer;
}

std::optional<std::reference_wrapper<XrCompositionLayerBaseHeader>> OverlayComponent::OpenXR::generate_slate_layer(
    runtimes::OpenXR::SwapchainIndex swapchain, 
    XrEyeVisibility eye)
{
    switch ((OverlayComponent::OverlayType)m_parent->m_slate_overlay_type->value()) {
    default:
    case OverlayComponent::OverlayType::QUAD:
        if (auto result = generate_slate_quad(swapchain, eye); result.has_value()) {
            return *(XrCompositionLayerBaseHeader*)&result.value().get();
        }

        return std::nullopt;
    case OverlayComponent::OverlayType::CYLINDER:
        if (!VR::get()->get_runtime()->is_cylinder_layer_allowed()) {
            if (auto result = generate_slate_quad(swapchain, eye); result.has_value()) {
                return *(XrCompositionLayerBaseHeader*)&result.value().get();
            }

            return std::nullopt;
        }

        if (auto result = generate_slate_cylinder(swapchain, eye); result.has_value()) {
            return *(XrCompositionLayerBaseHeader*)&result.value().get();
        }

        return std::nullopt;
    };
}


std::optional<std::reference_wrapper<XrCompositionLayerQuad>> OverlayComponent::OpenXR::generate_framework_ui_quad() {
    if (!g_framework->is_drawing_anything()) {
        m_parent->m_framework_intersect_state.intersecting = false;
        return std::nullopt;
    }

    auto& vr = VR::get();

    auto& layer = this->m_framework_ui_layer;

    layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    const auto& ui_swapchain = vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI];
    layer.subImage.swapchain = ui_swapchain.handle;
    layer.subImage.imageRect.offset.x = 0;
    layer.subImage.imageRect.offset.y = 0;
    layer.subImage.imageRect.extent.width = ui_swapchain.width;
    layer.subImage.imageRect.extent.height = ui_swapchain.height;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.eyeVisibility = XrEyeVisibility::XR_EYE_VISIBILITY_BOTH;

    auto glm_matrix = glm::identity<glm::mat4>();

    if (vr->m_overlay_component.m_framework_ui_follows_view->value()) {
        layer.space = vr->m_openxr->view_space;
    } else {
        auto rotation_offset = glm::inverse(vr->get_rotation_offset());

        // If we're not drawing the UI, this means we want to draw the cursor all the time
        // So we need to rotate the UI's pitch as well
        if (!g_framework->is_drawing_ui() && vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
            const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
            const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

            // Add the inverse of the pitch rotation to the rotation offset
            rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
        }

        glm_matrix = Matrix4x4f{rotation_offset};
        glm_matrix[3] += vr->get_standing_origin();
        layer.space = vr->m_openxr->stage_space;
    }

    const auto size_meters = g_framework->is_drawing_ui() ? m_parent->m_framework_size->value() : m_parent->m_slate_size->value();
    const float scale_factor =  g_framework->is_drawing_ui() ? ((float)ui_swapchain.width / 1920.0f) : 1.0f;

    // Adjust size_meters based on scaling factor.
    const float adjusted_size_meters = size_meters * scale_factor;

    // Compute the new dimensions in meters.
    const auto meters_w = (float)ui_swapchain.width / (float)ui_swapchain.height * adjusted_size_meters;
    const auto meters_h = adjusted_size_meters;

    layer.size = {meters_w, meters_h};

    if (g_framework->is_drawing_ui()) {
        glm_matrix[3] -= glm_matrix[2] * m_parent->m_framework_distance->value();
    } else {
        glm_matrix[3] -= glm_matrix[2] * (m_parent->m_slate_distance->value() - 0.01f);

        glm_matrix[3] += m_parent->m_slate_x_offset->value() * glm_matrix[0];
        glm_matrix[3] += m_parent->m_slate_y_offset->value() * glm_matrix[1];
    }

    glm_matrix[3].w = 1.0f;

    layer.pose.orientation = runtimes::OpenXR::to_openxr(glm::quat_cast(glm_matrix));
    layer.pose.position = runtimes::OpenXR::to_openxr(glm_matrix[3]);

    // Check if the controller pointer intersects with the quad, and we can use this to emulate the mouse
    if (vr->is_using_controllers()) {
        // Right only for now for testing
        const auto controller_index = !vr->m_swap_controllers->value() ? vr->get_right_controller_index() : vr->get_left_controller_index();
        const auto right_controller_rot = glm::quat{vr->get_rotation(controller_index, false)};
        const auto right_controller_pos = glm::vec3{vr->get_position(controller_index, false)};

        const auto start = right_controller_pos;
        auto fwd = (right_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f});
        const auto end = right_controller_pos + (fwd * 1000.0f);
        
        const auto plane_pos = glm::vec3{glm_matrix[3]};

        float intersection_distance = 0.0f;
        if (glm::intersectRayPlane<glm::vec3>(start, fwd, plane_pos, glm::normalize(glm::vec3{glm_matrix[2]}), intersection_distance)) {
            const auto intersection_point = start + (fwd * intersection_distance);

            const auto local_point = glm::inverse(glm_matrix) * glm::vec4{intersection_point, 1.0f};

            const auto w_half = meters_w / 2.0f;
            const auto h_half = meters_h / 2.0f;

            if (local_point.x >= -w_half && local_point.x <= w_half && local_point.y >= -h_half && local_point.y <= h_half) {
                const auto x = (local_point.x + w_half) / meters_w;
                const auto y = (meters_h - (local_point.y + h_half)) / meters_h;

                m_parent->m_framework_intersect_state.quad_intersection_point = {x, y};
                m_parent->m_framework_intersect_state.intersecting = true;

                if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI); it != vr->m_openxr->swapchains.end()) {
                    const auto client_x = (int32_t)((float)it->second.width * x);
                    const auto client_y = (int32_t)((float)it->second.height * y);

                    m_parent->m_framework_intersect_state.swapchain_intersection_point = {client_x, client_y};
                }
            } else {
                m_parent->m_framework_intersect_state.intersecting = false;
            }
        }
    } else {
        m_parent->m_framework_intersect_state.intersecting = false;
    }

    return layer;
}

// E-Day: the menu pointer's beams (thin quads from each controller to where it points, turned to the head) and dots (on
// the menu where a beam hits), from the beam ends edayMenuPointer worked out this frame. Quad layers in stage space,
// all of one small solid swapchain, submitted after the menu so they are drawn over it.
void OverlayComponent::OpenXR::append_eday_pointer_layers(std::vector<XrCompositionLayerBaseHeader*>& layers) {
    using namespace eday_menu;

    if (!enabled) {
        return;
    }

    auto& vr = VR::get();
    const auto sc_idx = (uint32_t)runtimes::OpenXR::SwapchainIndex::EDAY_BEAM;

    if (!vr->m_openxr->swapchains.contains(sc_idx)) {
        return;
    }

    const auto& sc = vr->m_openxr->swapchains[sc_idx];
    const glm::vec3 head{vr->get_position(0)};

    auto add = [&](XrCompositionLayerQuad& layer, const glm::vec3& xax, const glm::vec3& yax, const glm::vec3& zax, const glm::vec3& pos, float w, float h) {
        layer = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layer.space = vr->m_openxr->stage_space;
        layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        layer.subImage.swapchain = sc.handle;
        layer.subImage.imageRect.offset = {0, 0};
        layer.subImage.imageRect.extent = {(int32_t)sc.width, (int32_t)sc.height};
        layer.subImage.imageArrayIndex = 0;
        layer.pose.orientation = runtimes::OpenXR::to_openxr(glm::normalize(glm::quat_cast(glm::mat3{xax, yax, zax})));
        layer.pose.position = runtimes::OpenXR::to_openxr(pos);
        layer.size = {w, h};
        layers.push_back((XrCompositionLayerBaseHeader*)&layer);
    };

    for (int i = 0; i < 2; ++i) {
        if (!ray_ok[i]) {
            continue;
        }

        const glm::vec3 a{beam[i][0], beam[i][1], beam[i][2]}, b{beam[i][3], beam[i][4], beam[i][5]};
        const glm::vec3 d = b - a;
        const float len = glm::length(d);

        if (len < 0.01f) {
            continue;
        }

        // beam: y along it, z towards the head (as the OpenVR overlays)
        const glm::vec3 yax = d / len;
        const glm::vec3 mid = (a + b) * 0.5f;
        const glm::vec3 to_eye = head - mid;
        glm::vec3 zax = to_eye - yax * glm::dot(to_eye, yax);
        zax = glm::length(zax) < 1e-4f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::normalize(zax);
        const glm::vec3 xax = glm::normalize(glm::cross(yax, zax));
        add(m_eday_layers[i], xax, yax, zax, mid, EDAY_BEAM_W, len);

        // dot: facing the head, where the beam hits the menu
        if (hit_dist[i] >= 0.0f) {
            glm::vec3 dz = head - b;
            dz = glm::length(dz) < 1e-4f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::normalize(dz);
            glm::vec3 dx = glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, dz);
            dx = glm::length(dx) < 1e-4f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::normalize(dx);
            const glm::vec3 dy = glm::cross(dz, dx);
            add(m_eday_layers[2 + i], dx, dy, dz, b + dz * 0.002f, EDAY_DOT_W, EDAY_DOT_W);
        }
    }
}
}