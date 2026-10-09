# Lazy profile resolution for post authors

Post bodies can be rendered before their authors exist in `Storage`. `PostWidget` displays a fallback user ID and uses `UserProfileService::ensureUser()` to load the profile via batched `/users/ids` requests.

Profiles are not guaranteed to resolve in the first request: network errors, temporary backend failures and late cache population can leave a widget visible while a user is still unknown. The widget listens to `UserProfileService::profileResolved(userId)` for its author ID and updates the author label and avatar on later arrival, including profile renames. `resolveReferences()` updates `BackendPost::author` as well; changing that pointer alone is not enough to redraw an existing widget.

A transient unsuccessful `/users/ids` request must not immediately complete all waiters with null; the service retries the *same batch* with bounded backoff before completing callbacks. All IDs in a request stay in the in-flight set until a successful response or final failure. Avatar GET requests also retry with a bounded backoff and retain their in-flight key across retries to prevent a flood when many visible messages share an author.

The backend service's generation invalidates delayed retries after account/session reset. Do not let stale profile or avatar responses mutate the new account storage.

Tests: `UserProfileRecoveryTest.cpp` covers the UI rebinding path and retry after a temporary HTTP failure.
