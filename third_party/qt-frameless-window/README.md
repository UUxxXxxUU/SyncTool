# Qt-FramelessWindow

- Upstream: https://github.com/winsoft666/Qt-FramelessWindow
- Pinned commit: `326a15011a87d776a4a40f4bb395b62b15dd1e22`
- License: MIT; see `LICENSE`.
- Original header SHA256: `f930a7aafb6b2ea7e3a8f51d7b73d4efc57b0a62feb7630aa60baaba4ea6ac6e`.

Local adjustments qualify dependent-base calls with `this->` for standard C++ template lookup.
The application adapter in `src/roundedwindow.h` supplies the title bar and rounded background.
Native Aero Snap and native system shadows are not provided by this component.
