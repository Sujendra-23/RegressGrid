package store

import (
	"context"
	"encoding/json"
	"errors"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/redis/go-redis/v9"
	"os"
	"path/filepath"
	"time"
)

// One control-plane replica owns this snapshot. Logs/artifacts are separate files.
type Store struct {
	DB    *pgxpool.Pool
	Redis *redis.Client
	Dir   string
}

func Open(ctx context.Context, dir, dsn, redisURL string) (*Store, error) {
	s := &Store{Dir: dir}
	if err := os.MkdirAll(dir, 0750); err != nil {
		return nil, err
	}
	if dsn != "" {
		db, err := pgxpool.New(ctx, dsn)
		if err != nil {
			return nil, err
		}
		s.DB = db
		if _, err = db.Exec(ctx, `CREATE TABLE IF NOT EXISTS grid_state (id integer PRIMARY KEY CHECK(id=1), data jsonb NOT NULL, updated_at timestamptz NOT NULL DEFAULT now())`); err != nil {
			db.Close()
			return nil, err
		}
	}
	if redisURL != "" {
		opt, err := redis.ParseURL(redisURL)
		if err != nil {
			s.Close()
			return nil, err
		}
		s.Redis = redis.NewClient(opt)
		if err = s.Redis.Ping(ctx).Err(); err != nil {
			s.Close()
			return nil, err
		}
	}
	return s, nil
}
func (s *Store) Load(ctx context.Context) ([]byte, error) {
	if s.DB != nil {
		var b []byte
		err := s.DB.QueryRow(ctx, `SELECT data FROM grid_state WHERE id=1`).Scan(&b)
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return b, err
	}
	b, err := os.ReadFile(filepath.Join(s.Dir, "state.json"))
	if os.IsNotExist(err) {
		return nil, nil
	}
	return b, err
}
func (s *Store) Save(ctx context.Context, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	if s.DB != nil {
		_, err = s.DB.Exec(ctx, `INSERT INTO grid_state (id,data) VALUES (1,$1) ON CONFLICT(id) DO UPDATE SET data=excluded.data,updated_at=now()`, b)
		return err
	}
	path := filepath.Join(s.Dir, "state.json")
	f, err := os.CreateTemp(s.Dir, "state-*")
	if err != nil {
		return err
	}
	name := f.Name()
	defer os.Remove(name)
	if _, err = f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return os.Rename(name, path)
}
func (s *Store) Queue(ctx context.Context, ids []string) error {
	if s.Redis == nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(ctx, time.Second)
	defer cancel()
	// Redis is a reconstructible scheduling index; durable state is authoritative.
	_, err := s.Redis.TxPipelined(ctx, func(p redis.Pipeliner) error {
		p.Del(ctx, "regressgrid:queue")
		for n, id := range ids {
			p.ZAdd(ctx, "regressgrid:queue", redis.Z{Score: float64(n), Member: id})
		}
		return nil
	})
	return err
}
func (s *Store) QueueIDs(ctx context.Context) ([]string, error) {
	if s.Redis == nil {
		return nil, nil
	}
	ctx, cancel := context.WithTimeout(ctx, time.Second)
	defer cancel()
	return s.Redis.ZRange(ctx, "regressgrid:queue", 0, -1).Result()
}
func (s *Store) Close() {
	if s.DB != nil {
		s.DB.Close()
	}
	if s.Redis != nil {
		s.Redis.Close()
	}
}
