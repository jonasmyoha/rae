// The spawn benchmarks of benchmarks/spawn/rae/Main.rae on tokio tasks: a
// multi-thread runtime with WORKERS worker threads (default: every core).
// Same modes, same data, same algorithms; prints RESULT,tokio,... lines.
// The fork-join sum shares the numbers through an Arc (what tokio code does);
// the merge sort copies its halves, like the Rae version.
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::time::{Duration, Instant};

fn result(benchmark: &str, parameter: usize, metric: &str, value: f64) {
    println!("RESULT,tokio,{benchmark},{parameter},{metric},{value}");
}

fn median(mut values: Vec<f64>) -> f64 {
    values.sort_by(|a, b| a.partial_cmp(b).unwrap());
    values[values.len() / 2]
}

fn numbers_of(count: usize) -> Vec<i64> {
    let mut state: i64 = 12345;
    (0..count)
        .map(|_| {
            state = (state * 1103515245 + 12345) % 2147483648;
            state % 1000
        })
        .collect()
}

async fn spawn_join(count: usize) {
    let mut total: i64 = 0;
    let start = Instant::now();
    for i in 0..count as i64 {
        total += tokio::spawn(async move { i + 1 }).await.unwrap();
    }
    let one = start.elapsed().as_secs_f64() * 1e6 / count as f64;
    result("spawnJoin", count, "oneAtATimeUsPerTask", one);
    let start = Instant::now();
    let mut done = 0;
    while done < count {
        let batch = 256.min(count - done);
        let handles: Vec<_> = (0..batch).map(|k| tokio::spawn(async move { (done + k) as i64 + 1 })).collect();
        for handle in handles {
            total += handle.await.unwrap();
        }
        done += batch;
    }
    result("spawnJoin", count, "batched256UsPerTask", start.elapsed().as_secs_f64() * 1e6 / count as f64);
    result("spawnJoin", count, "checksum", total as f64);
}

async fn sleeps(count: usize) {
    let start = Instant::now();
    let handles: Vec<_> = (0..count)
        .map(|_| {
            tokio::spawn(async {
                tokio::time::sleep(Duration::from_millis(100)).await;
                1
            })
        })
        .collect();
    let spawned = start.elapsed().as_secs_f64() * 1000.0;
    let mut finished = 0;
    for handle in handles {
        finished += handle.await.unwrap();
    }
    result("sleeps", count, "spawnAllMs", spawned);
    result("sleeps", count, "wallMs", start.elapsed().as_secs_f64() * 1000.0);
    result("sleeps", count, "finished", finished as f64);
}

fn report(name: &str, mut samples: Vec<u128>, count: usize) {
    samples.sort();
    let at = |fraction: f64| samples[((samples.len() - 1) as f64 * fraction) as usize] as f64 / 1000.0;
    result("latency", count, &format!("{name}P50Us"), at(0.5));
    result("latency", count, &format!("{name}P90Us"), at(0.9));
    result("latency", count, &format!("{name}P99Us"), at(0.99));
    result("latency", count, &format!("{name}MaxUs"), at(1.0));
}

async fn latency(count: usize) {
    let mut starts = Vec::with_capacity(count);
    let mut round_trips = Vec::with_capacity(count);
    for _ in 0..count {
        let before = Instant::now();
        let running = tokio::spawn(async { Instant::now() }).await.unwrap();
        let after = Instant::now();
        starts.push((running - before).as_nanos());
        round_trips.push((after - before).as_nanos());
    }
    report("spawnToRunning", starts, count);
    report("spawnToGet", round_trips, count);
}

fn sequential_sum(numbers: &[i64]) -> i64 {
    numbers.iter().sum()
}

fn parallel_sum(numbers: Arc<Vec<i64>>, start: usize, count: usize, leaf: usize) -> Pin<Box<dyn Future<Output = i64> + Send>> {
    Box::pin(async move {
        if count <= leaf {
            return sequential_sum(&numbers[start..start + count]);
        }
        let half = count / 2;
        let left = tokio::spawn(parallel_sum(numbers.clone(), start, half, leaf));
        let right = parallel_sum(numbers, start + half, count - half, leaf).await;
        left.await.unwrap() + right
    })
}

async fn sum(count: usize, leaf: usize) {
    let numbers = Arc::new(numbers_of(count));
    let expected = sequential_sum(&numbers);
    let (mut sequential_times, mut fork_times, mut wrong) = (vec![], vec![], 0);
    for round in 0..6 {
        let start = Instant::now();
        if std::hint::black_box(sequential_sum(&numbers)) != expected {
            wrong += 1;
        }
        let sequential_ms = start.elapsed().as_secs_f64() * 1000.0;
        let start = Instant::now();
        if parallel_sum(numbers.clone(), 0, count, leaf).await != expected {
            wrong += 1;
        }
        let fork_ms = start.elapsed().as_secs_f64() * 1000.0;
        if round > 0 {
            sequential_times.push(sequential_ms);
            fork_times.push(fork_ms);
        }
    }
    result("sum", leaf, "sequentialMs", median(sequential_times));
    result("sum", leaf, "forkJoinMs", median(fork_times));
    result("sum", leaf, "leaves", (count / leaf) as f64);
    result("sum", leaf, "wrong", wrong as f64);
}

fn merge(left: &[i64], right: &[i64]) -> Vec<i64> {
    let mut merged = Vec::with_capacity(left.len() + right.len());
    let (mut i, mut j) = (0, 0);
    while i < left.len() && j < right.len() {
        if left[i] <= right[j] {
            merged.push(left[i]);
            i += 1;
        } else {
            merged.push(right[j]);
            j += 1;
        }
    }
    merged.extend_from_slice(&left[i..]);
    merged.extend_from_slice(&right[j..]);
    merged
}

fn sequential_sort(numbers: Vec<i64>) -> Vec<i64> {
    if numbers.len() <= 32 {
        let mut sorted = numbers;
        sorted.sort();
        return sorted;
    }
    let half = numbers.len() / 2;
    let left = sequential_sort(numbers[..half].to_vec());
    let right = sequential_sort(numbers[half..].to_vec());
    merge(&left, &right)
}

fn parallel_sort(numbers: Vec<i64>, leaf: usize) -> Pin<Box<dyn Future<Output = Vec<i64>> + Send>> {
    Box::pin(async move {
        if numbers.len() <= leaf {
            return sequential_sort(numbers);
        }
        let half = numbers.len() / 2;
        let left = tokio::spawn(parallel_sort(numbers[..half].to_vec(), leaf));
        let right = parallel_sort(numbers[half..].to_vec(), leaf).await;
        merge(&left.await.unwrap(), &right)
    })
}

async fn sort(count: usize, leaf: usize) {
    let numbers = numbers_of(count);
    let (mut sequential_times, mut fork_times, mut wrong) = (vec![], vec![], 0);
    for round in 0..4 {
        let start = Instant::now();
        let sequential = sequential_sort(numbers.clone());
        let sequential_ms = start.elapsed().as_secs_f64() * 1000.0;
        let start = Instant::now();
        let parallel = parallel_sort(numbers.clone(), leaf).await;
        let fork_ms = start.elapsed().as_secs_f64() * 1000.0;
        if parallel != sequential || parallel.windows(2).any(|pair| pair[0] > pair[1]) {
            wrong += 1;
        }
        if round > 0 {
            sequential_times.push(sequential_ms);
            fork_times.push(fork_ms);
        }
    }
    result("sort", leaf, "sequentialMs", median(sequential_times));
    result("sort", leaf, "forkJoinMs", median(fork_times));
    result("sort", leaf, "leaves", (count / leaf) as f64);
    result("sort", leaf, "wrong", wrong as f64);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let number = |index: usize| args.get(index).and_then(|text| text.parse::<usize>().ok()).unwrap_or(0);
    let workers = std::env::var("WORKERS")
        .ok()
        .and_then(|text| text.parse().ok())
        .unwrap_or_else(|| std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4));
    let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(workers).enable_all().build().unwrap();
    let mode = args.get(1).map(String::as_str).unwrap_or("");
    runtime.block_on(async {
        match mode {
            "spawnJoin" => spawn_join(number(2)).await,
            "sleeps" => sleeps(number(2)).await,
            "latency" => latency(number(2)).await,
            "sum" => sum(number(2), number(3)).await,
            "sort" => sort(number(2), number(3)).await,
            _ => eprintln!("usage: spawnJoin <count> | sleeps <count> | latency <samples> | sum <count> <leaf> | sort <count> <leaf>"),
        }
    });
}
