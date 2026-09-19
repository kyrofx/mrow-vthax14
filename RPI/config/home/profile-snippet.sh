# --- BiteDJ appliance session ---
# Appended to ~/.profile. getty@tty1 autologins into that shell; this hands the
# box straight to sway, which brings up waybar, udiskie and BiteDJ itself.
#
# Guarded on VT 1 so that SSH logins (and any other VT) still get an ordinary
# shell -- you are never locked out by a broken session config.
#
# Note ~/.profile is used because neither ~/.bash_profile nor ~/.bash_login
# exists. If you create either, bash stops reading ~/.profile and the session
# will silently never start.
if [ -z "$WAYLAND_DISPLAY" ] && [ "$XDG_VTNR" = "1" ]; then
    export XDG_CURRENT_DESKTOP=sway
    # This Pi exposes two DRM cards: card0 is v3d (render-only) and card1 is
    # vc4-drm (display-only). wlroots otherwise advertises the display card to
    # clients, which has no render capability, so Mesa silently falls back to
    # llvmpipe -- GBM reports V3D while the Wayland platform reports llvmpipe.
    # Point it at the v3d render node so clients get hardware GL.
    export WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128
    export XDG_SESSION_TYPE=wayland
    exec sway >"$HOME/.local/share/sway.log" 2>&1
fi
