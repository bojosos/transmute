# UserNotifications

`/System/Library/Frameworks/UserNotifications.framework/Versions/A/UserNotifications` (alchemy-fx).

* **Real:** Content, actions, categories, requests, attachments, responses and settings are real value objects; the notification center is a singleton that keeps its delegate and categories.
* **Unavailable, as documented:** Notifications are not allowed: authorization is denied (`UNAuthorizationStatusDenied`, every setting disabled), and adding a request fails with `UNErrorCodeNotificationsNotAllowed` (1). Nothing was delivered or is pending. Alchemy has no notification server; host toast notifications (a `host::ui` call over Windows toasts) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_usernotifications`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/usernotifications_tests.cpp` (ctest `alchemy_gui_usernotifications`).
