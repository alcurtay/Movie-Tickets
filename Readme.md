# Movie Booking (in-memory)
A minimal, cross-platform C++ 11 library and test app for listing movies/theaters/shows and atomically booking seats — all in memory (no DB). The domain model and API live in C++
A small test harness in `main()` exercises seat discovery and booking (including a simple concurrency test). 

## What’s implemented
* Data model (in memory)
  * `Theater` : name, capacity, vector of `ShowInfo` (each show tracks price, start time, remaining seats, and a per-seat bitmap) 
  * `ShowInfo` : `movieName`, `start (time_t)`, `price`, `freeTickets`, `taken` flags (size = capacity) 
  * `ShowSeatsAvailable` : per-show list of free seat IDs (`A1…A<N>`) with start time and price 

* Service interface
  * `IBookingService` abstracts queries and booking across many theaters:
    * `addTheater(name, capacity)`
    * `addShowInfo(theater, movie, std::tm, price)` and `addShowInfo(theater, movie, time_t, price)`
    * `listMovies(day)`
    * `selectMovie(movie, day)` : `{ theater -> vector<ShowInfo> }`
    * `listTheatersShowingMovie(movie, day)`
    * `selectTheater(theater, day)` : shows for that day
    * `seatsAvailable(theater, movie, day)` : vector of `{start, price, seats[]}`
    * `bookSeats(theater, movie, dt, seatIds, show_no)` (atomic, mutex-protected) 

* Thread-safety / atomic booking
  * `Theater::bookSeats(...)` validates and books all requested seats inside a mutex. If any seat is invalid/already taken, booking fails and nothing changes (all-or-nothing). 

* Dates & “today”
  * Helpers `toLocalMidnight(time_t)` and `getTodaysDate(h,m)` make “same calendar day” comparisons robust and timezone-correct. 

* Unit-style tests in `main()`
  * Seat discovery, successful booking, double-booking prevention, and a tiny two-thread race that proves only one thread can grab the same seat. Look for `[OK] ... passed.` in stdout. 

* Doxygen-ready comments
  * Public methods are annotated for doc generation. 


## Build
### Linux / macOS (no external deps required)
bash commands
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/booking

### Windows (MSVC)
powershell
vcpkg install boost-program-options:x64-windows
vcpkg integrate install
cmake -S . -B build `
  -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
.\build\Release\booking.exe

The sample `CMakeLists.txt` has details to build on both Linux (with and without boost library) & Windows.

## Usage (programmatic)
Typical flow:
movie.cpp
MovieBookingService svc;
svc.addTheater("Apsara", 30);

auto tm1930 = /* std::tm for today 19:30 */;
svc.addShowInfo("Apsara", "Inception", tm1930, 15.00);

// List movies today
auto movies = svc.listMovies();              // ["Inception", ...]

// See shows by theater
auto shows  = svc.selectTheater("Apsara");   // vector<ShowInfo>

// Check seats
auto avail  = svc.seatsAvailable("Apsara", "Inception", getTodaysDate(19, 30));
for (auto& sh : avail) {
  // sh.seats is a vector<string> like {"A1","A2","A3"...}
}

// Book seats atomically (by HH:MM match on the given day)
bool ok = svc.bookSeats("Apsara","Inception", getTodaysDate(19,30),
                        {"A1","A2"}, /*show_no=*/0);


Seat IDs: single-row scheme `"A1"..."A<capacity>"`. Mapping is done via helper functions. Invalid or already booked IDs cause the call to fail atomically. 

## Key API Notes
* Day filtering: All “on day” queries compare by local date after normalizing to midnight (`toLocalMidnight`). Time-of-day is ignored unless you use `bookSeats(..., show_no=0)` which matches exact HH:MM. 
* show_no semantics: `0` = match HH:MM; `>0` = choose the 1-based N-th show that day ordered by start time. 
* Threading: `Theater` guards booking with a `std::mutex`. If you add theaters at runtime from multiple threads, also guard the container in `MovieBookingService`. 

## Tests
The `main()` function runs two suites:
* runSeatTests() — seat discovery, successful booking, duplicate booking rejection, simple two-thread race (only one wins).
* runServiceTests() — service-level seat queries + book + verify seats disappear.

When all test pass, you will get output as


[OK] Seat availability & booking tests passed.
[OK] Service-level seat APIs passed.


## Doxygen
Generate HTML docs (requires Doxygen):

bash commands
doxygen -g            # once, creates Doxyfile
doxygen Doxyfile      # outputs docs/html

Public methods and structs are annotated (`@brief`, params, returns, notes). 

## Next steps (optional)
* Add a real CLI on top of `IBookingService` (the commented CMake shows how you’d link Boost.Program_options). 
* Persist/seed shows from JSON (still keep memory as source of truth).
* Extend seat layout beyond a single row (`A1..A<N>`).

## Files
* `movie.cpp` — domain model, service interface + implementation, tests in `main()`. 
* `CMakeLists.txt` (sample) — minimal build; commented guidance for a future CLI target. 
* `Readme.md` — this file. (Updated to reflect current code.) 
