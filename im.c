#include <errno.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "wlchewing.h"

static void noop() {
	// no-op
}

bool im_key_press(struct wlchewing_state *state, xkb_keysym_t keysym) {
	// return false if unhandled, need forwarding
	if (xkb_state_mod_name_is_active(state->xkb_state, XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_EFFECTIVE) > 0) {
		if (keysym == XKB_KEY_space) {
			// Chinese / English(forwarding) toggle
			state->forwarding = !state->forwarding;
			chewing_Reset(state->chewing);
			if (state->bottom_panel) {
				bottom_panel_destroy(state->bottom_panel);
				state->bottom_panel = NULL;
			}
			zwp_input_method_v2_set_preedit_string(
				state->input_method, "", 0, 0);
			zwp_input_method_v2_commit(state->input_method,
				state->serial);
			return true;
		}
		return false;
	}
	// TODO check other modifiers not used by us
	if (state->forwarding) {
		return false;
	}
	if (state->bottom_panel) {
		switch(keysym){
		case XKB_KEY_KP_Enter:
		case XKB_KEY_Return:
			chewing_cand_choose_by_index(state->chewing,
				state->bottom_panel->selected_index);
			chewing_cand_close(state->chewing);
			bottom_panel_destroy(state->bottom_panel);
			state->bottom_panel = NULL;
			break;
		case XKB_KEY_Left:
			if (state->bottom_panel->selected_index > 0) {
				state->bottom_panel->selected_index--;
				bottom_panel_render(state->bottom_panel,
					state->chewing);
				wl_display_roundtrip(state->display);
			}
			break;
		case XKB_KEY_Right:
			printf("chewing report %d\n",
				chewing_cand_TotalChoice(state->chewing));
			if (state->bottom_panel->selected_index
					< chewing_cand_TotalChoice(
						state->chewing) - 1) {
				state->bottom_panel->selected_index++;
				bottom_panel_render(state->bottom_panel,
					state->chewing);
				wl_display_roundtrip(state->display);
			}
			break;
		case XKB_KEY_Up:
			chewing_cand_close(state->chewing);
			bottom_panel_destroy(state->bottom_panel);
			state->bottom_panel = NULL;
			break;
		case XKB_KEY_Down:
			if (chewing_cand_list_has_next(state->chewing)) {
				chewing_cand_list_next(state->chewing);
			} else {
				chewing_cand_list_first(state->chewing);
			}
			state->bottom_panel->selected_index = 0;
			bottom_panel_render(state->bottom_panel,
				state->chewing);
			wl_display_roundtrip(state->display);
			break;
		default:
			// no-op
			break;
		}
	} else {
		// TODO if chewing preedit is empty, forward bs, del, arrows...
		switch(keysym){
		case XKB_KEY_BackSpace:
			chewing_handle_Backspace(state->chewing);
			break;
		case XKB_KEY_Delete:
			chewing_handle_Del(state->chewing);
			break;
		case XKB_KEY_KP_Enter:
		case XKB_KEY_Return:
			chewing_handle_Enter(state->chewing);
			break;
		case XKB_KEY_Left:
			chewing_handle_Left(state->chewing);
			break;
		case XKB_KEY_Right:
			chewing_handle_Right(state->chewing);
			break;
		case XKB_KEY_Down:
			chewing_cand_open(state->chewing);
			if (chewing_cand_TotalChoice(state->chewing)) {
				state->bottom_panel = bottom_panel_new(state);
				bottom_panel_render(state->bottom_panel,
					state->chewing);
				wl_display_roundtrip(state->display);
			}
			break;
		default:
			chewing_handle_Default(state->chewing,
				(char)xkb_keysym_to_utf32(keysym));
		}
	}

	char *preedit = chewing_buffer_String(state->chewing);
	if (!strlen(preedit)) {
		free(preedit);
		preedit = strdup(chewing_bopomofo_String_static(state->chewing));
	}
	else {
		preedit = strcat(preedit, chewing_bopomofo_String_static(
			state->chewing));
	}
	int cursor_begin = chewing_cursor_Current(state->chewing);
	zwp_input_method_v2_set_preedit_string(state->input_method, preedit,
		cursor_begin, cursor_begin);
	if (chewing_commit_Check(state->chewing)) {
		zwp_input_method_v2_commit_string(state->input_method, 
			chewing_commit_String_static(state->chewing));
	}
	zwp_input_method_v2_commit(state->input_method, state->serial);
	return true;
}

static void handle_key(void *data, struct zwp_input_method_keyboard_grab_v2
		*zwp_input_method_keyboard_grab_v2, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t key_state) {
	struct wlchewing_state *state = data;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state,
		key + 8);
	char keysym_name[64];
	xkb_keysym_get_name(keysym, keysym_name, sizeof(keysym_name));
	printf(" xkb translated to %s\n", keysym_name);
	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (!im_key_press(state, keysym)){
			// wlchewing_err("Forwarding not yet implemeted");
			// forward, record that future release needs forwarding
			zwp_virtual_keyboard_v1_key(state->virtual_keyboard,
				time, key, key_state);
			// TODO release on real release
			zwp_virtual_keyboard_v1_key(state->virtual_keyboard,
				time, key, WL_KEYBOARD_KEY_STATE_RELEASED);
		} else if (state->kb_rate != 0) {
			state->last_keysym = keysym;
			struct itimerspec timer_spec = {
				.it_interval = {
					.tv_sec = 0,
					.tv_nsec = 1000000000 / state->kb_rate,
				},
				.it_value = {
					.tv_sec = 0,
					.tv_nsec = state->kb_delay * 1000000,
				},
			};
			if (timerfd_settime(state->timer_fd, 0, &timer_spec,
					NULL) == -1) {
				wlchewing_err("Failed to arm timer: %s",
					strerror(errno));
			}
		}
	} else {
		if (keysym == state->last_keysym) {
			state->last_keysym = 0;
			struct itimerspec timer_disarm = {0};
			if (timerfd_settime(state->timer_fd, 0, &timer_disarm,
					NULL) == -1) {
				wlchewing_err("Failed to disarm timer: %s",
					strerror(errno));
			}
			return;
		}
		// forward if needs forwarding
	}
}

static void handle_modifiers(void *data, struct zwp_input_method_keyboard_grab_v2
		*zwp_input_method_keyboard_grab_v2, uint32_t serial,
		uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	struct wlchewing_state *state = data;
	xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
		mods_locked, 0, 0, group);
	// forward modifiers
	zwp_virtual_keyboard_v1_modifiers(state->virtual_keyboard,
		mods_depressed, mods_latched, mods_locked, group);
}

static void handle_keymap(void *data, struct zwp_input_method_keyboard_grab_v2
		*zwp_input_method_keyboard_grab_v2, uint32_t format,
		int32_t fd, uint32_t size) {
	struct wlchewing_state *state = data;
	char *keymap_string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(
		state->xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(keymap_string, size);
	//close(fd);
	xkb_state_unref(state->xkb_state);
	state->xkb_state = xkb_state_new(keymap);
	xkb_keymap_unref(keymap);
	// forward keymap
	zwp_virtual_keyboard_v1_keymap(state->virtual_keyboard,
		format, fd, size);
}

static void handle_repeat_info(void *data,
		struct zwp_input_method_keyboard_grab_v2
		*zwp_input_method_keyboard_grab_v2, int32_t rate,
		int32_t delay) {
	struct wlchewing_state *state = data;
	state->kb_delay = delay;
	state->kb_rate = rate;
}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
	.key = handle_key,
	.modifiers = handle_modifiers,
	.keymap = handle_keymap,
	.repeat_info = handle_repeat_info,
};

static void handle_activate(void *data,
		struct zwp_input_method_v2 *zwp_input_method_v2) {
	struct wlchewing_state *state = data;
	state->pending_activate = true;
}

static void handle_deactivate(void *data,
		struct zwp_input_method_v2 *zwp_input_method_v2) {
	struct wlchewing_state *state = data;
	state->pending_activate = false;
}

static void handle_unavailable(void *data,
		struct zwp_input_method_v2 *zwp_input_method_v2) {
	struct wlchewing_state *state = data;
	wlchewing_err("IM unavailable");
	im_destory(state);
	exit(EXIT_FAILURE);
}

static void handle_done(void *data,
		struct zwp_input_method_v2 *zwp_input_method_v2) {
	struct wlchewing_state *state = data;
	state->serial++;
	if (state->pending_activate && !state->activated) {
		state->kb_grab = zwp_input_method_v2_grab_keyboard(
			state->input_method);
		zwp_input_method_keyboard_grab_v2_add_listener(state->kb_grab,
			&grab_listener, state);
		wl_display_roundtrip(state->display);

		if (!state->kb_grab) {
			wlchewing_err("Failed to grab");
			exit(EXIT_FAILURE);
		}
	} else if (!state->pending_activate && state->activated) {
		zwp_input_method_keyboard_grab_v2_release(state->kb_grab);
		state->kb_grab = NULL;
		if (state->bottom_panel) {
			bottom_panel_destroy(state->bottom_panel);
			state->bottom_panel = NULL;
		}
		chewing_Reset(state->chewing);
	}
	state->activated = state->pending_activate;
}

static const struct zwp_input_method_v2_listener im_listener = {
	.activate = handle_activate,
	.deactivate = handle_deactivate,
	.surrounding_text = noop,
	.text_change_cause = noop,
	.content_type = noop,
	.done = handle_done,
	.unavailable = handle_unavailable,
};

/* TODO for adding panel with input-method support
 * currently only panel with wlr-layer-shell
 */

void im_setup(struct wlchewing_state *state) {
	state->input_method = zwp_input_method_manager_v2_get_input_method(
		state->input_method_manager, state->seat);
	zwp_input_method_v2_add_listener(state->input_method, &im_listener, state);

	state->virtual_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		state->virtual_keyboard_manager, state->seat);

	state->chewing = chewing_new();
	state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	wl_display_roundtrip(state->display);
}

void im_destory(struct wlchewing_state *state) {
	chewing_delete(state->chewing);
	xkb_state_unref(state->xkb_state);
	xkb_context_unref(state->xkb_context);
	zwp_input_method_v2_destroy(state->input_method);
	if (state->bottom_panel) {
		bottom_panel_destroy(state->bottom_panel);
		state->bottom_panel = NULL;
	}
}
