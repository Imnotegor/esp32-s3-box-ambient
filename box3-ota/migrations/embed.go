// Package migrations embeds the ordered SQLite schema migrations.
package migrations

import "embed"

// Files contains the immutable migration scripts.
//
//go:embed *.sql
var Files embed.FS
