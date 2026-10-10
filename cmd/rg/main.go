package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run() error {
	server := flag.String("server", "http://localhost:8080", "control plane URL")
	token := flag.String("token", os.Getenv("API_TOKEN"), "API bearer token (or API_TOKEN)")
	flag.Parse()
	a := flag.Args()
	if len(a) == 0 {
		return fmt.Errorf("usage: rg [flags] submit request.json | status [build-id] | cancel build-id | promote build-id version | logs job-id")
	}
	method, path, body := "GET", "/api/state", []byte(nil)
	switch a[0] {
	case "submit":
		if len(a) != 2 {
			return fmt.Errorf("submit needs a JSON file")
		}
		var err error
		body, err = os.ReadFile(a[1])
		if err != nil {
			return err
		}
		method, path = "POST", "/api/builds"
	case "status":
		if len(a) == 2 {
			path = "/api/builds/" + a[1]
		}
	case "cancel", "retry":
		if len(a) != 2 {
			return fmt.Errorf("%s needs a build id", a[0])
		}
		method, path = "POST", "/api/builds/"+a[1]+"/"+a[0]
	case "promote":
		if len(a) != 3 {
			return fmt.Errorf("promote needs build-id and version")
		}
		method, path = "POST", "/api/builds/"+a[1]+"/promote"
		body, _ = json.Marshal(map[string]string{"version": a[2]})
	case "logs":
		if len(a) != 2 {
			return fmt.Errorf("logs needs a job id")
		}
		path = "/api/jobs/" + a[1] + "/logs"
	default:
		return fmt.Errorf("unknown command %s", a[0])
	}
	req, err := http.NewRequest(method, strings.TrimRight(*server, "/")+path, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if *token != "" {
		req.Header.Set("Authorization", "Bearer "+*token)
	}
	client := http.Client{Timeout: 30 * time.Second}
	res, err := client.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	b, err := io.ReadAll(io.LimitReader(res.Body, 16<<20))
	if err != nil {
		return err
	}
	if res.StatusCode >= 400 {
		return fmt.Errorf("%s: %s", res.Status, b)
	}
	var formatted bytes.Buffer
	if json.Indent(&formatted, b, "", "  ") == nil {
		fmt.Println(formatted.String())
	} else {
		fmt.Print(string(b))
	}
	return nil
}
