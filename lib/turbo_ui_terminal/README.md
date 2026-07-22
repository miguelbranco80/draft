# turbo_ui_terminal

`turbo_ui_terminal` is the small adapter from `core/terminal` key, mouse, and
resize observations to `turbo_ui.Event`. It has no event loop, renderer,
application state, or compiler knowledge; callers remain responsible for input
timing and terminal resource lifetime.
