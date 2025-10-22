#include <boost/program_options.hpp>
#include <iostream>
#include <list>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <mutex>
#include <iterator>
#include <cassert>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <cstdlib>

namespace po = boost::program_options;
using DateTime = std::tm;
using namespace std;

const int defaultTheaterCapacity = 20;

/*
 * @brief Get a timestamp for today's local date at the given hour and minute.
 * @param h Hour in [0, 23].
 * @param m Minute in [0, 59].
 * @return std::time_t representing today at h:m:00 (local time).
 * @throws std::out_of_range if h or m are out of range.
 */
inline std::time_t getTodaysDate(int h = 0, int m = 0)
{
    if (h < 0 || h > 23 || m < 0 || m > 59)
        throw std::out_of_range("hour must be 0..23, minute 0..59");

    std::time_t now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &now_t);
#else
    localtime_r(&now_t, &tm_local);
#endif
    tm_local.tm_hour = h;
    tm_local.tm_min  = m;
    tm_local.tm_sec  = 0;

    return std::mktime(&tm_local);
}

/*
 * @brief Normalize a timestamp to local midnight (00:00:00) for its day.
 * @param t Input timestamp.
 * @return std::time_t at 00:00:00 local time of the same calendar day.
 */
inline std::time_t toLocalMidnight(std::time_t t) {
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    tm_local.tm_hour = 0;
    tm_local.tm_min = 0;
    tm_local.tm_sec = 0;
    return std::mktime(&tm_local);
}

/*
 * @brief Concrete show instance with title, start time, price, and remaining seats.
 * @details Equality compares (movieName, start) only.
 */
struct ShowInfo
{
    std::string       movieName;
    std::time_t       start;
    double            price = 0.0;
    int               freeTickets = 0;
	std::vector<bool> taken;

    /// @brief Default constructor.
    ShowInfo() = default;

    /*
     * @brief Construct a show info.
     * @param name Movie title.
     * @param stime Start time (local).
     * @param price Ticket price.
     * @param seats Initial available seats.
     */
    ShowInfo(std::string name, std::time_t stime, double price, int seats)
      : movieName(std::move(name)), start(stime), price(price), freeTickets(seats) {}

    /*
     * @brief Equality operator by (movieName, start).
     * @param rhs Right-hand side.
     * @return true if same movie and same start time.
     */
    bool operator==(const ShowInfo& rhs) const noexcept
    {
        return movieName == rhs.movieName && start == rhs.start;
    }
};

/// On a given day, report per-show seat availability.
struct ShowSeatsAvailable
{
    std::time_t              start;
    double                   price{};
    std::vector<std::string> seats;
    ShowSeatsAvailable(std::time_t s, double p, std::vector<std::string> ids)
        : start{s}, price{p}, seats(std::move(ids)) {}
};

/// @brief Hour:Minute pair extracted from a timestamp.
struct HM { int h; int m; };

/*
 * @brief Extract local hour/minute from a timestamp.
 * @param t Input timestamp.
 * @return HM with hour (0..23) and minute (0..59).
 */
inline HM hour_min_local(std::time_t t)
{
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    return HM{ tm_local.tm_hour, tm_local.tm_min };
}

/*
 * @class Theater
 * @brief In-memory catalog of shows for a single theater, with seat booking.
 * @details
 *   - “Today” queries compare by local date (midnight-normalized).
 *   - Booking is mutex-protected to avoid over-booking.
 *   - Supports time-match mode (`show_no==0`) and 1-based ordinal mode.
 */
class Theater
{
    std::string    theaterName;
    int            maxSeats = defaultTheaterCapacity;
    std::vector<ShowInfo> vShowInfo;
    mutable std::mutex mtx_;

	/// Convert 0-based index -> "A1".."A{maxSeats}"
	static std::string makeSeatId(int idx)
	{
		/// Single-row model "A"
		return std::string("A") + std::to_string(idx + 1);
	}

	/// Convert "A1".."A{maxSeats}" -> 0-based index; returns -1 if invalid/out-of-range
	int seatIndexFromId(const std::string& id) const
	{
		if (id.size() < 2 || (id[0] != 'A' && id[0] != 'a')) return -1;
		char* endp = nullptr;
		long n = std::strtol(id.c_str() + 1, &endp, 10);
		if (*endp != '\0' || n <= 0 || n > maxSeats) return -1;
		return static_cast<int>(n - 1);
	}

public:
    /*
     * @brief Construct a Theater with a name and capacity.
     * @param name Theater name.
     * @param seats Maximum seats per show (default 20).
     */
    Theater(std::string name, int seats=defaultTheaterCapacity)
      : theaterName(std::move(name)), maxSeats(seats)
    {
        vShowInfo.reserve(50);
    }

    /*
     * @brief Disable copy constructor.
     */
    Theater(const Theater&) = delete;

    /*
     * @brief Disable copy operator.
     */
    Theater& operator=(const Theater&) = delete;

    /*
     * @brief Move constructor (mutex is default-constructed fresh)
     * @param other object to be moved constructed from.
     */
    Theater(Theater&& other) noexcept
        : theaterName(std::move(other.theaterName)),
          maxSeats(other.maxSeats),
          vShowInfo(std::move(other.vShowInfo)) {
    }

    /*
     * @brief Move operator.
     * @param other Theater object.
     */
    Theater& operator=(Theater&& other) noexcept
	{
        if (this != &other) {
            theaterName = std::move(other.theaterName);
            maxSeats    = other.maxSeats;
            vShowInfo   = std::move(other.vShowInfo);
        }
        return *this;
    }

    /*
     * @brief Get the theater's name.
     * @return Theater name.
     */
    std::string getTheaterName() const { return theaterName; }

    /*
     * @brief Add a show using a local calendar time (std::tm).
     * @param name Movie title.
     * @param stime Local calendar time; converted to time_t via mktime.
     * @param price Ticket price.
     * @note Initializes freeTickets to theater capacity. Uses mutex for thread-safety.
     */
    void addShowInfo(const std::string& name, DateTime stime, double price)
    {
        std::time_t start_tt = std::mktime(&stime);    // local
        std::lock_guard<std::mutex> lk(mtx_);          // lock if this races with reads
        vShowInfo.emplace_back(name, start_tt, price, this->maxSeats);
		vShowInfo.back().taken.assign(static_cast<size_t>(this->maxSeats), false);
		vShowInfo.back().freeTickets = this->maxSeats;
    }

    /*
     * @brief Add a show using a ready timestamp (time_t).
     * @param name Movie title.
     * @param start_tt Start time (local).
     * @param price Ticket price.
     * @note Initializes freeTickets to theater capacity. Uses mutex for thread-safety.
     */
    void addShowInfo(const std::string& name, std::time_t start_t, double price)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        vShowInfo.emplace_back(name, start_t, price, this->maxSeats);
		vShowInfo.back().taken.assign(static_cast<size_t>(this->maxSeats), false);
		vShowInfo.back().freeTickets = this->maxSeats;
	}

	/*
	 * @brief Check whether a movie has at least one show on a given day.
	 * @param movieName Movie title to check.
	 * @param day Local timestamp for the target calendar day (time-of-day ignored).
	 *            Defaults to std::time(nullptr) (i.e., “today”).
	 * @return true if there is at least one show for the movie on that day; false otherwise.
	 */
	bool hasShowOnDay(const std::string& movieName, std::time_t day = std::time(nullptr)) const
	{
		const std::time_t day0 = toLocalMidnight(day);

		return std::any_of(vShowInfo.begin(), vShowInfo.end(),
			[&](const ShowInfo& s) {
				return s.movieName == movieName &&
					   toLocalMidnight(s.start) == day0;
			});
	}

	/*
	 * @brief List the free seat IDs for a specific movie show.
	 * @param moviename Movie title (exact match).
	 * @param start     Start time (exact time_t match for the show).
	 * @return Vector of free seat IDs (e.g., {"A1","A2"}). Empty if not found or no seats.
	 * @note This is a read-only view. If concurrent writers exist, guard with the theater mutex.
	 */
	std::vector<std::string> availableSeatIds(const std::string& moviename, std::time_t start) const
	{
		std::vector<std::string> ids;
		for (const auto& s : vShowInfo) {
			if (s.movieName == moviename && s.start == start) {
				ids.reserve(static_cast<size_t>(maxSeats));
				for (int i = 0; i < maxSeats; ++i)
					if (i < static_cast<int>(s.taken.size()) && !s.taken[i])
						ids.push_back(makeSeatId(i));
				break;
			}
		}
		return ids;
	}

	/*
	 * @brief Get unique, sorted list of movie titles showing on a given date.
	 * @param day A timestamp for the target date (local). Time-of-day is ignored.
	 * @return Vector of titles (sorted ascending, duplicates removed).
	 */
	std::vector<std::string> getMovieListOn(std::time_t day = std::time(nullptr)) const
	{
		const std::time_t day0 = toLocalMidnight(day);
		std::vector<std::string> names;
		names.reserve(vShowInfo.size());

		for (const auto& s : vShowInfo)
			if (toLocalMidnight(s.start) == day0)
				names.push_back(s.movieName);

		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		return names;
	}

	/*
	 * @brief Get all shows that occur on a given day (any title).
	 * @param day Local timestamp for the target calendar day (time-of-day ignored).
	 *            Defaults to std::time(nullptr) (i.e., “today”).
	 * @return Vector of ShowInfo copies scheduled on that day.
	 */
	std::vector<ShowInfo> getListOfShowsOn(std::time_t day = std::time(nullptr)) const
	{
		const std::time_t day0 = toLocalMidnight(day);
		std::vector<ShowInfo> movieShows;
		movieShows.reserve(vShowInfo.size());

		std::copy_if(vShowInfo.begin(), vShowInfo.end(), std::back_inserter(movieShows),
					 [&](const ShowInfo& s){ return toLocalMidnight(s.start) == day0; });

		return movieShows;
	}

	/*
	 * @brief Get all shows for a specific movie on a given day.
	 * @param moviename Title to filter on.
	 * @param day Local timestamp for the target calendar day (time-of-day ignored).
	 *            Defaults to std::time(nullptr) (i.e., “today”).
	 * @return Vector of ShowInfo copies for that movie on that day.
	 */
	std::vector<ShowInfo> getListofMovieShowsOn(const std::string& moviename,
												   std::time_t day = std::time(nullptr)) const
	{
		const std::time_t day0 = toLocalMidnight(day);
		std::vector<ShowInfo> movieShows;
		movieShows.reserve(vShowInfo.size());

		for (const auto& s : vShowInfo)
			if (s.movieName == moviename && toLocalMidnight(s.start) == day0)
				movieShows.push_back(s);

		return movieShows;
	}

	/*
	 * @brief Atomically book specific seat IDs for the chosen show.
	 * @param moviename Movie title.
	 * @param dt        Target date/time:
	 *                  - If show_no == 0: select show by exact HH:MM on that date.
	 *                  - If show_no > 0 : select the show_no-th show (1-based) on that date, ordered by start time.
	 * @param seatIds   Seat IDs to book (e.g., {"A2","A3"}). All IDs must be valid and free.
	 * @param show_no   0 for time-match mode; >0 for ordinal mode.
	 * @return true if booking succeeds (all seats booked); false otherwise.
	 * @threadsafe Booking and revalidation are protected by an internal mutex.
	 */
	bool bookSeats(const std::string& moviename,
				   std::time_t dt,
				   const std::vector<std::string>& seatIds,
				   int show_no = 0)
	{
		if (seatIds.empty()) return false;
		const std::time_t targetDate = toLocalMidnight(dt);
		std::vector<size_t> candidates;
		candidates.reserve(vShowInfo.size());

		for (size_t i = 0; i < vShowInfo.size(); ++i) {
			const ShowInfo& s = vShowInfo[i];
			if (s.movieName == moviename && toLocalMidnight(s.start) == targetDate)
				candidates.push_back(i);
		}
		if (candidates.empty()) return false;

		size_t chosenIdx = static_cast<size_t>(-1);
		if (show_no == 0) {
			const HM targetHM = hour_min_local(dt);
			for (size_t idx : candidates) {
				const HM hm = hour_min_local(vShowInfo[idx].start);
				if (hm.h == targetHM.h && hm.m == targetHM.m) { chosenIdx = idx; break; }
			}
			if (chosenIdx == static_cast<size_t>(-1)) return false;
		} else {
			if (show_no <= 0) return false;
			std::sort(candidates.begin(), candidates.end(),
					  [&](size_t a, size_t b){ return vShowInfo[a].start < vShowInfo[b].start; });
			if (static_cast<size_t>(show_no) > candidates.size()) return false;
			chosenIdx = candidates[static_cast<size_t>(show_no) - 1];
		}

		std::lock_guard<std::mutex> lk(mtx_);
		if (chosenIdx >= vShowInfo.size()) return false;
		ShowInfo& show = vShowInfo[chosenIdx];

		if (show.movieName != moviename || toLocalMidnight(show.start) != targetDate)
			return false;
		if (show_no == 0) {
			const HM targetHM = hour_min_local(dt);
			const HM hm = hour_min_local(show.start);
			if (hm.h != targetHM.h || hm.m != targetHM.m) return false;
		}

		std::vector<int> idxs;
		idxs.reserve(seatIds.size());
		for (const auto& id : seatIds) {
			int idx = seatIndexFromId(id);
			if (idx < 0 || idx >= maxSeats) return false;
			if (idx >= static_cast<int>(show.taken.size())) return false;
			if (show.taken[idx]) return false; // already booked
			idxs.push_back(idx);
		}

		for (int idx : idxs) show.taken[idx] = true;
		show.freeTickets -= static_cast<int>(idxs.size());
		return true;
	}
};


// --------------------- Service interface --------------------
/*
 * @class IBookingService
 * @brief Abstract interface for querying movies/theaters/shows and booking tickets.
 *
 * Implementations aggregate one or more Theater catalogs and provide
 * query/booking operations over them.
 */
class IBookingService {
public:
	/*
	 * @brief Add a new theater entry if it does not already exist.
	 * @param theater  Theater name.
	 * @param capacity Seating capacity. defaults to defaultTheaterCapacity.
	 * @note If a theater with the same name already exists, this is a no-op.
	 */
    virtual void addTheater(const std::string& theater, int capacity = defaultTheaterCapacity) = 0;

    /*
     * @brief Add a show using a local calendar time (std::tm).
     * @param theater Theater name object created if it does not exist.
     * @param movie   Movie name.
     * @param stime   Local calendar time. converted to time_t via std::mktime.
     * @param price   Ticket price.
     * @return void.
     * @note Implementations should initialize per-show free seats to the theater capacity.
     */
    virtual void addShowInfo(const std::string& theater,
                             const std::string& movie,
                             DateTime stime,
                             double price) = 0;

    /*
     * @brief Add a show using a ready timestamp (time_t).
     * @param theater Theater name object created if it does not exist.
     * @param movie   Movie name.
     * @param start_tt Start time (local).
     * @param price   Ticket price.
     * @return void.
     * @note Implementations should initialize per-show free seats to the theater capacity.
     */
    virtual void addShowInfo(const std::string& theater,
                             const std::string& movie,
                             std::time_t start_tt,
                             double price) = 0;

    /*
     * @brief List unique movie titles playing on a given day across all theaters.
     * @param day Local timestamp for the target calendar day. time ignored if provided.
     *            Defaults to std::time(nullptr) (“today”).
     * @return Sorted, de-duplicated vector of movie titles.
     */
    virtual std::vector<std::string> listMovies(std::time_t day = std::time(nullptr)) const = 0;

    /*
     * @brief For a given movie, list all shows per theater on a given day.
     * @param movie Movie title to search.
     * @param day   Local timestamp for the target calendar day time ignored.
     * @return Map: theater name -> vector of ShowInfo for that day.
     */
    virtual std::unordered_map<std::string, std::vector<ShowInfo>>
			selectMovie(const std::string& movie,  std::time_t day = std::time(nullptr)) const = 0;

    /*
     * @brief List theater names that are showing a movie on a given day.
     * @param movie Movie title.
     * @param day   Local timestamp for the target calendar day (time ignored).
     * @return Sorted (and de-duplicated) vector of theater names. Empty if no matches.
     */
    virtual std::vector<std::string>
		listTheatersShowingMovie(const std::string& movie, std::time_t day = std::time(nullptr)) const = 0;

    /*
     * @brief Get all shows in a given theater on a given day (any title).
     * @param theater Theater name (exact match).
     * @param day     Local timestamp for the target calendar day (time ignored).
     * @return All ShowInfo instances scheduled that day for the theater, sorted by start; empty if none or not found.
     */
    virtual std::vector<ShowInfo>
		selectTheater(const std::string& theater, std::time_t day = std::time(nullptr)) const = 0;

    /*
     * @brief For a movie in a theater on a given day, report per-show seat availability.
     * @param theater Theater name.
     * @param movie   Movie title.
     * @param day     Local timestamp for the target calendar day (time ignored).
     * @return Vector of ShowSeatsAvailable (only entries with freeTickets > 0). Sorted by start time. Empty if none.
     */
    virtual std::vector<ShowSeatsAvailable> seatsAvailable(const std::string& theater,
											   const std::string& movie,
											   std::time_t day = std::time(nullptr)) const = 0;

	/*
	 * @brief Attempt to book specific seat IDs for a movie in a theater/show.
	 * @param theater   Theater name.
	 * @param moviename Movie title.
	 * @param dt        Target date/time (match by HH:MM if show_no==0).
	 * @param seatIds   List of seat IDs to book (e.g., {"A2","A3"}).
	 * @param show_no   0 for time-match; >0 use 1-based ordinal that day (sorted by start).
	 * @return true on success, false if any seat was invalid/unavailable or show not found.
	 */
	virtual bool bookSeats(const std::string& theater,
						   const std::string& moviename,
						   std::time_t dt,
						   const std::vector<std::string>& seatIds,
						   int show_no = 0) = 0;

    /// @brief Virtual destructor.
    virtual ~IBookingService() = default;
};

// ------------------ Stub implementation -----------------------
/*
 * @class MovieBookingService
 * @brief Basic in-memory implementation of IBookingService over a set of Theater objects.
 *
 * @details
 *  - Aggregates multiple Theater catalogs.
 *  - Provides cross-theater queries and booking delegation to Theater.
 *  - Thread-safety of vTheater container itself is not provided here; seat
 *    booking within Theater is protected by its internal mutex.
 */
class MovieBookingService : public IBookingService {
    std::vector<Theater> vTheater;

public:
    /// @brief Construct with a small initial capacity for theaters.
    MovieBookingService()
	{
        vTheater.reserve(defaultTheaterCapacity);
    }

    /*
     * IBookingService::addTheater(const std::string&, int)
     */
	void addTheater(const std::string& theater, int capacity) override
	{
		auto it = find_if(vTheater.begin(), vTheater.end(),
				[&](const Theater& th){ return th.getTheaterName() == theater; });
		if (it == vTheater.end())
			vTheater.emplace_back(theater, capacity);
	}

    /*
     * IBookingService::addShowInfo(const std::string&, const std::string&, DateTime, double)
     */
    void addShowInfo(const std::string& theater, const std::string& movie, DateTime stime, double price) override
    {
		auto it = std::find_if(vTheater.begin(), vTheater.end(),
				[&](const Theater& th) { return th.getTheaterName() == theater; });
		if (it == vTheater.end()) {
			vTheater.emplace_back(theater);
			it = std::prev(vTheater.end());
		}
		it->addShowInfo(movie, stime, price);
    }

    /*
     * IBookingService::addShowInfo(const std::string&, const std::string&, std::time_t, double)
     */
    void addShowInfo(const std::string& theater, const std::string& movie, std::time_t start_t, double price) override
    {
		auto it = std::find_if(vTheater.begin(), vTheater.end(),
				[&](const Theater& th) { return th.getTheaterName() == theater; });
		if (it == vTheater.end()) {
			vTheater.emplace_back(theater);
			it = std::prev(vTheater.end());
		}
		it->addShowInfo(movie, start_t, price);
    }

    /*
     * IBookingService::listMovies
     */
    std::vector<std::string> listMovies(std::time_t day) const override
	{
        std::vector<std::string> movies;
        movies.reserve(vTheater.size() * 4);  // heuristic

        for (const auto& t : vTheater) {
            auto titles = t.getMovieListOn(day);
            movies.insert(movies.end(), titles.begin(), titles.end());
        }

        std::sort(movies.begin(), movies.end());
        movies.erase(std::unique(movies.begin(), movies.end()), movies.end());
        // movies.shrink_to_fit(); // optional
        return movies;
    }

    /*
     * IBookingService::selectMovie
     */
    std::unordered_map<std::string, std::vector<ShowInfo>>
		selectMovie(const std::string& movie, std::time_t day) const override
	{
        std::unordered_map<std::string, std::vector<ShowInfo>> result;
        result.reserve(vTheater.size()); // at most one entry per theater

        for (const auto& t : vTheater) {
            auto shows = t.getListofMovieShowsOn(movie, day); // NOTE: matches Theater API
            if (!shows.empty()) {
                result.emplace(t.getTheaterName(), std::move(shows));
            }
        }
        return result;
    }

    /*
     * IBookingService::listTheatersShowingMovie
     */
    std::vector<std::string> listTheatersShowingMovie(const std::string& movie, std::time_t day) const override
	{
        std::vector<std::string> theaters;
        theaters.reserve(vTheater.size());

        for (const auto& t : vTheater) {
            if (t.hasShowOnDay(movie, day)) {
                theaters.emplace_back(t.getTheaterName());
            }
        }

        std::sort(theaters.begin(), theaters.end());
        theaters.erase(std::unique(theaters.begin(), theaters.end()), theaters.end());
        return theaters;
    }

    /*
     * IBookingService::selectTheater
     */
    std::vector<ShowInfo> selectTheater(const std::string& theater, std::time_t day) const override
	{
        auto it = std::find_if(vTheater.begin(), vTheater.end(),
                               [&](const Theater& th){ return th.getTheaterName() == theater; });
        if (it == vTheater.end())
            return {};
        auto shows = it->getListOfShowsOn(day);
        std::sort(shows.begin(), shows.end(),
                  [](const ShowInfo& a, const ShowInfo& b){ return a.start < b.start; });
        return shows;
    }

    /*
     * IBookingService::seatsAvailable
	 * @details For each matching show, returns the list of free seat IDs at query time.
     */
    std::vector<ShowSeatsAvailable> seatsAvailable(const std::string& theater,
														   const std::string& movie,
														   std::time_t day) const override
	{
        auto it = std::find_if(vTheater.begin(), vTheater.end(),
                               [&](const Theater& th){ return th.getTheaterName() == theater; });
        if (it == vTheater.end())
            return {};
        auto shows = it->getListofMovieShowsOn(movie, day);
        std::vector<ShowSeatsAvailable> tickets;
        tickets.reserve(shows.size());
		for (const auto& s : shows) {
			std::vector<std::string> freeIds = it->availableSeatIds(movie, s.start);
			if (!freeIds.empty()) {
				tickets.emplace_back(s.start, s.price, std::move(freeIds));
			} else {
				tickets.emplace_back(s.start, s.price, std::move(freeIds));
			}
		}
		std::sort(tickets.begin(), tickets.end(),
				  [](const ShowSeatsAvailable& a, const ShowSeatsAvailable& b){
					  return a.start < b.start;
				  });
        return tickets;
    }

    /*
     * IBookingService::bookSeats
     */
	bool bookSeats(const std::string& theater,
				   const std::string& moviename,
				   std::time_t dt,
				   const std::vector<std::string>& seatIds,
				   int show_no) override
	{
		auto it = std::find_if(vTheater.begin(), vTheater.end(),
							   [&](Theater& th){ return th.getTheaterName() == theater; });
		if (it == vTheater.end())
			return false;
		return it->bookSeats(moviename, dt, seatIds, show_no);
	}

    /// @brief Defaulted destructor.
    ~MovieBookingService() = default;
};

/*
 * @brief Build a local calendar time for today at (h:m).
 * @param h Hour [0,23].
 * @param m Minute [0,59].
 * @return std::tm for today's date at (h:m).
 */
static DateTime make_today_tm(int h, int m)
{
    std::time_t now = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    lt.tm_hour = h;
    lt.tm_min  = m;
    lt.tm_sec  = 0;
    return lt;
}

/*
 * @brief Seat-level tests for Theater: discovery, booking, and race on same seat.
 */
static void runSeatTests()
{
    Theater th("Apsara", 6); // small capacity for test
    // One show today at 18:00
    auto tm18 = make_today_tm(18, 0);
    th.addShowInfo("Inception", tm18, 12.50);

    // Initially, all seats free
    {
        auto avail = th.availableSeatIds("Inception", std::mktime(&tm18));
        assert(avail.size() == 6);
        // Expect A1..A6
        for (int i=0;i<6;++i) assert(avail[i] == ("A" + std::to_string(i+1)));
    }

    // Book A2,A3 (time-match mode show_no=0)
    {
        bool ok = th.bookSeats("Inception", getTodaysDate(18,0), std::vector<std::string>{"A2","A3"}, 0);
        assert(ok);
        auto avail = th.availableSeatIds("Inception", std::mktime(&tm18));
        // A2,A3 removed -> 4 left
        assert(avail.size() == 4);
        for (auto& s : avail) assert(s != "A2" && s != "A3");
    }

    // Attempt to re-book an already taken seat
    {
        bool ok = th.bookSeats("Inception", getTodaysDate(18,0), std::vector<std::string>{"A3"}, 0);
        assert(!ok);
    }

    // Simple concurrency: two threads try to book A4 at the same time; only one should succeed
    {
        std::atomic<int> successes{0};
        auto try_book_A4 = [&](){
            if (th.bookSeats("Inception", getTodaysDate(18,0), std::vector<std::string>{"A4"}, 0))
                ++successes;
        };
        std::thread t1(try_book_A4), t2(try_book_A4);
        t1.join();
		t2.join();
        assert(successes.load() == 1);

        auto avail = th.availableSeatIds("Inception", std::mktime(&tm18));
        for (auto& s : avail) assert(s != "A4");
    }
    std::cout << "[OK] Seat availability & booking tests passed.\n";
}

/*
 * @brief Service-level tests for seatsAvailable() and bookSeats().
 */
static void runServiceTests()
{
    MovieBookingService svc;
    svc.addTheater("Apsara", 6);
    svc.addShowInfo("Apsara", "Inception", make_today_tm(19, 30), 15.0);
    svc.addShowInfo("Apsara", "Inception", make_today_tm(21, 00), 15.0);

    // Seats listing
    auto avail = svc.seatsAvailable("Apsara", "Inception", getTodaysDate(19, 30));
    assert(!avail.empty());
    for (auto& sh : avail) {
        assert(!sh.seats.empty());
        // Book first two seats of the first show
    }
    // Book two seats on the first show (time-match)
    {
        auto s = avail.front();
        // Parse hour/minute of s.start
        HM hm = hour_min_local(s.start);
        bool ok = svc.bookSeats("Apsara","Inception", getTodaysDate(hm.h, hm.m),
                                std::vector<std::string>{"A1","A2"}, 0);
        assert(ok);

        // Seats should be gone now
        auto again = svc.seatsAvailable("Apsara", "Inception", getTodaysDate(21, 00));
        // Find the same start
        auto it = std::find_if(again.begin(), again.end(),
                               [&](const ShowSeatsAvailable& x){ return x.start == s.start; });
        assert(it != again.end());
        for (auto& sid : it->seats) {
            assert(sid != "A1" && sid != "A2");
        }
    }
    std::cout << "[OK] Service-level seat APIs passed.\n";
}

int main() {
    runSeatTests();
    runServiceTests();
    return 0;
}
