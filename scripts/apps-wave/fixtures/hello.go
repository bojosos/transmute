package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"
)

func main() {
	var wg sync.WaitGroup
	results := make([]int, 8)
	for i := range results {
		wg.Add(1)
		go func(i int) { defer wg.Done(); results[i] = i * i }(i)
	}
	wg.Wait()
	sum := 0
	for _, v := range results {
		sum += v
	}
	fmt.Printf("hello from go %s %s/%s, sum=%d, args=%d\n", runtime.Version(), runtime.GOOS, runtime.GOARCH, sum, len(os.Args))
}
