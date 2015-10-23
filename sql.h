#ifndef SQL_H
#define SQL_H

#include <tuple>
#include <vector>
#include <set>
#include <iostream>
#include <stdexcept>
#include <memory>

#include <chrono>

#include <sqlite3.h>

//#define SQL_TRACE

template<class T>
static T get_sql_as_type(sqlite3_stmt *const stmt, int col);

template<>
unsigned get_sql_as_type<unsigned>(sqlite3_stmt *const stmt, int col)
{
	return sqlite3_column_int(stmt, col);
}
template<>
std::int32_t get_sql_as_type<std::int32_t>(sqlite3_stmt *const stmt, int col)
{
	return sqlite3_column_int(stmt, col);
}

template<>
std::string get_sql_as_type<std::string>(sqlite3_stmt *const stmt, int col)
{
	const char *bytes = static_cast<const char*>(sqlite3_column_blob(stmt, col));
	const int count = sqlite3_column_bytes(stmt, col);
	return std::string(bytes, count);
}

template<>
std::vector<unsigned char> get_sql_as_type<std::vector<unsigned char>>(sqlite3_stmt *const stmt, int col)
{
	const unsigned char *bytes = static_cast<const unsigned char*>(sqlite3_column_blob(stmt, col));
	const int count = sqlite3_column_bytes(stmt, col);
	return std::vector<unsigned char>(bytes, bytes+count);
}

template<>
std::shared_ptr<std::string> get_sql_as_type<std::shared_ptr<std::string>>(sqlite3_stmt *const stmt, int col)
{
	if (SQLITE_NULL == sqlite3_column_type(stmt, col))
		return nullptr;
	else
		return std::make_shared<std::string>(get_sql_as_type<std::string>(stmt, col));
}


template<>
double get_sql_as_type<double>(sqlite3_stmt *const stmt, int col)
{
	return sqlite3_column_double(stmt, col);
}

template<>
std::uint64_t get_sql_as_type<std::uint64_t>(sqlite3_stmt *const stmt, int col)
{
	return sqlite3_column_int64(stmt, col);
}

template<>
std::int64_t get_sql_as_type<std::int64_t>(sqlite3_stmt *const stmt, int col)
{
	return sqlite3_column_int64(stmt, col);
}

template<int i, class Tuple, class A, typename ...Tail>
struct nextCol
{
	static void col(Tuple &t, sqlite3_stmt *const stmt)
	{
		const int cols = sqlite3_column_count(stmt);
		if (i < cols)
		{
			std::get<i>(t) = get_sql_as_type<A>(stmt, i);
		}
		else
			std::get<i>(t) = typename std::tuple_element<i,Tuple>::type();
		nextCol<i+1, Tuple, Tail...>::col(t, stmt);
	}
};

template<int i, class Tuple>
struct nextCol<i, Tuple, void>
{
	static void col(Tuple &, sqlite3_stmt *const )
	{
	}
};

template<typename ...T>
class Args
{
public:
	typedef std::tuple<T...> tuple;
	
	static void col(std::tuple<T...> &row, sqlite3_stmt *const stmt)
	{
		nextCol<0, std::tuple<T...>, T..., void>::col(row, stmt);
	}
};

template<typename ...T>
class Args<std::tuple<T...>>
{
public:
	typedef std::tuple<T...> tuple;
	
	static void col(std::tuple<T...> &row, sqlite3_stmt *const stmt)
	{
		nextCol<0, std::tuple<T...>, T..., void>::col(row, stmt);
	}
};


class no_rows : public std::runtime_error
{
public:
	no_rows() : std::runtime_error("No value") { }
};

class Sql
{
public:
	struct LastRegexp;
private:
	sqlite3 *db;
public:
	Sql();
	~Sql();


	template<typename ...Cols>
	struct RowType
	{
		typedef ::Args<Cols...> args;
		typedef typename args::tuple tuple;
	};
	

	class Statement
	{
		friend class Sql;
		struct BoundValueDeleter;
		template<typename V>
		struct BoundValueDeleterT;
		struct StatementPrivate
		{
			int refs;
			sqlite3_stmt *stmt;
			std::vector<BoundValueDeleter*> parameters;

			std::string statement;
			Sql *db;

		} *shared;

	public:
		Statement(const Statement &copy);

		Statement();
		~Statement();
		Statement& operator=(const Statement &copy);

		Statement &arg(const std::string &d);
		Statement &argBlob(const std::string &d);
		Statement &argBlob(const std::vector<std::uint8_t> &d);
		Statement &argBlob(const std::uint8_t *bytes, unsigned num);
		Statement &arg(const std::uint64_t &d);
		Statement &arg(const std::int64_t &d);
		Statement &arg(unsigned int d) { return arg(std::uint64_t(d)); }
		Statement &arg(int d) { return arg(std::int64_t(d)); }
		Statement &arg(const double &d);
		Statement &argZero(int numBytes);
		Statement &argNull();
		template<class Function, class Params>
		inline std::int64_t exec(const Params &, const Function &function);
		std::int64_t exec();
		template<typename T>
		T execValue();
		
		template<typename Tuple>
		Tuple execTuple();
		
		template<typename ...TupleTypes>
		std::tuple<TupleTypes...> execTypes();

		bool operator !() const { return !shared; }

	private:
		std::string error();
		std::string boundValues();

	private:
		void clearParameters();
	};

	sqlite3 *sqlite() { return db; }

	enum Options
	{
		Sql_NotWAL=0,
		Sql_WAL=1,
		Sql_NoCreate=2
	};

	void open(const std::string &database, int opt=Sql_WAL);
	
	bool isOpen() const { return !!db; }

	Statement statement(const std::string &sql);

	template<typename T>
	T execValue(const std::string &s)
	{
		return statement(s).execValue<T>();
	}

	bool hasTable(const std::string &name);

	static std::string escape(const std::string &s);

	std::int64_t exec(const std::string &s)
	{
		return statement(s).exec();
	}
	std::int64_t exec(const char *s)
	{
		return statement(std::string(s)).exec();
	}
	template<class T>
	std::int64_t exec(const std::string &s, T &function)
	{
		return statement(s).exec(function);
	}
	template<class T>
	std::int64_t exec(const char *s, T &function)
	{
		return statement(std::string(s)).exec(function);
	}
};

template<>
std::uint64_t Sql::Statement::execValue<std::uint64_t>();

template<>
unsigned Sql::Statement::execValue<unsigned>();

template<>
std::string Sql::Statement::execValue<std::string>();

template<typename Tuple>
inline Tuple Sql::Statement::execTuple()
{
	Tuple v;
	bool got=false;

	exec(
		Args<Tuple>(),
		[&] (const Tuple&t)
		{ got=true; v = t; }
	);
	if (!got)
		throw no_rows();
	return v;
}


template<typename ...TupleTypes>
inline std::tuple<TupleTypes...> Sql::Statement::execTypes()
{
	std::tuple<TupleTypes...> v;
	bool got=false;

	exec(
		Args<TupleTypes...>(),
		[&] (const std::tuple<TupleTypes...>&t)
		{ got=true; v = t; }
	);
	if (!got)
		throw no_rows();
	return v;
}


template<class Function, class Params>
inline std::int64_t Sql::Statement::exec(const Params &, const Function &function)
{
#ifdef SQL_TRACE
	std::cerr << "Executing: " << shared->statement << "(" << boundValues() << ")" << std::endl;
	
	std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
#endif

	sqlite3_stmt *const stmt = shared->stmt;
	int x;
	
	typename Params::tuple row;
	
	while (1)
	{
		x = sqlite3_step(stmt);
		if (x == SQLITE_ROW)
		{
			Params::col(row, stmt);

			try
			{
				function(row);
			}
			catch (std::exception &e)
			{
				clearParameters();
				throw std::runtime_error(
						"Error processing query <<<" + shared->statement + ">>>("
						+ boundValues() + "): " + e.what()
					);
			}
			catch (...)
			{
				clearParameters();
				throw;
			}
		}
		else if (x == SQLITE_BUSY)
			continue;
		else
			break;
	}

	if (x != SQLITE_OK && x != SQLITE_DONE)
	{
		std::runtime_error e(error());
		clearParameters();

		throw e;
	}

	clearParameters();

#ifdef SQL_TRACE
	std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
	std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
//	std::cerr << "query took " << time_span.count() << " seconds" << std::endl;
#endif
	return sqlite3_last_insert_rowid(shared->db->db);
}




#endif
// kate: space-indent off; replace-tabs off;

