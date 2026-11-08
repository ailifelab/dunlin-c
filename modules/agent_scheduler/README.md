# Durable Agent scheduler

Independent C11/SQLite state machine. It does not execute models, grant permissions, start threads or access SwiftUI. The host binds a principal, model reference, chat and canonical workspace resource identity before enqueueing. Parallel claims use SQLite immediate transactions; shared capacity, one-active-job-per-chat and reader/writer resource exclusion are enforced across connections. A task ID is idempotent only for the same immutable request. Nested tasks are limited to depth 8. Cancellation cascades; running leases remain held until completion. Explicit exclusive recovery marks active tasks interrupted and cancels their queued descendants; it never replays tools.

The desktop worker supervisor and native collaboration page are still separate unfinished integration work. Callers must not open a second `dc_store` owner or share its connection concurrently. A passing queue test is not multi-Agent product completion.
