#include "sql.h"

#include <sstream>

Sql::Sql()
{
	db = nullptr;
}

Sql::~Sql()
{
	if (db && SQLITE_BUSY==sqlite3_close(db))
	{
		std::cerr << "Warning: sqlite db not closed, statements not finalized" << std::endl;
	}
}

void Sql::open(const std::string &database, int opt)
{
	if (db)
		throw std::runtime_error("Sql object already used");
	
	static bool x = false;
	if (!x)
	{
		sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
		x = true;
	}

	int rc = sqlite3_open_v2(
			database.c_str(), &db,
			SQLITE_OPEN_READWRITE | (opt & Sql_NoCreate ? 0 : SQLITE_OPEN_CREATE) | SQLITE_OPEN_NOMUTEX,
			0
		);
	if (rc != SQLITE_OK)
	{
		throw std::runtime_error("Failed to open " + database);
	}
	if (0 == sqlite3_threadsafe())
	{
		throw std::runtime_error("Failing because sqlite is not threadsafe");
	}
	
	exec("PRAGMA page_size = 512");
	exec("PRAGMA legacy_file_format = 0");
	if (opt & Sql_WAL)
		exec("PRAGMA journal_mode=WAL");
}



std::int64_t Sql::Statement::exec()
{
	return exec(Args<>(), [] (const std::tuple<>&) { });
}

template<>
std::uint64_t Sql::Statement::execValue<std::uint64_t>()
{
	const std::string x = execValue<std::string>();
	try
	{
		return std::stoll(x);
	}
	catch (std::bad_cast&)
	{
		throw std::runtime_error("No value");
	}
}
template<>
unsigned Sql::Statement::execValue<unsigned>()
{
	const std::string x = execValue<std::string>();
	try
	{
		return std::stoul(x);
	}
	catch (std::bad_cast&)
	{
		throw std::runtime_error("No value");
	}
}
template<>
int Sql::Statement::execValue<int>()
{
	const std::string x = execValue<std::string>();
	try
	{
		return std::stol(x);
	}
	catch (std::bad_cast&)
	{
		throw std::runtime_error("No value");
	}
}

template<>
std::string Sql::Statement::execValue<std::string>()
{
	std::string v;
	bool got=false;

	exec(Args<std::string>(), [&] (const std::tuple<std::string>&t) { got=true; v = std::get<0>(t); });
	if (!got)
		throw no_rows();
	return v;
}


Sql::Statement::Statement()
{
	shared = 0;
}

Sql::Statement::Statement(const Sql::Statement &copy)
{
	if ((shared = copy.shared))
		shared->refs++;
}

Sql::Statement::~Statement()
{
	if (shared && (--shared->refs == 0))
	{
		clearParameters();
		sqlite3_finalize(shared->stmt);
		delete shared;
	}
}

Sql::Statement& Sql::Statement::operator=(const Sql::Statement &copy)
{
	if (copy.shared != this->shared)
	{
		if (shared && (--shared->refs == 0))
		{
			clearParameters();
			sqlite3_finalize(shared->stmt);
			delete shared;
		}
		shared = copy.shared;
		if (shared)
			shared->refs++;
	}
	return *this;
}


struct Sql::Statement::BoundValueDeleter
{
	virtual ~BoundValueDeleter() { }
	virtual std::string asString() const=0;
};

namespace
{
std::string expressBoundAsString(const std::string &v)
{
	std::string s = "\"";
	s += v;
	s += "\"";
	return s;
}
std::string expressBoundAsString(const std::vector<std::uint8_t> &v)
{
	std::string s = "(" + std::to_string(v.size()) + " bytes";
	return s;
}
template<typename T>
std::string expressBoundAsString(const T &v)
{
	return std::to_string(v);
}

}


template<typename V>
struct Sql::Statement::BoundValueDeleterT : public Sql::Statement::BoundValueDeleter
{
	const V value;
	inline BoundValueDeleterT(const V&v) : value(v) {}
	virtual std::string asString() const
	{
		return expressBoundAsString(value);
	}
};


Sql::Statement &Sql::Statement::arg(const std::string &d)
{
	BoundValueDeleterT<std::string> *del = new BoundValueDeleterT<std::string>(d);
	shared->parameters.push_back(del);

	sqlite3_bind_text(
			shared->stmt, shared->parameters.size(),
			del->value.data(), del->value.length(), 0
		);
	return *this;
}

Sql::Statement &Sql::Statement::argBlob(const std::string &d)
{
	BoundValueDeleterT<std::string> *del = new BoundValueDeleterT<std::string>(d);
	shared->parameters.push_back(del);

	sqlite3_bind_blob(
			shared->stmt, shared->parameters.size(),
			del->value.data(), del->value.length(), 0
		);
	return *this;
}

Sql::Statement &Sql::Statement::argBlob(const std::vector<std::uint8_t> &d)
{
	BoundValueDeleterT<std::vector<std::uint8_t> > *del = new BoundValueDeleterT<std::vector<std::uint8_t> >(d);
	shared->parameters.push_back(del);

	sqlite3_bind_blob(
			shared->stmt, shared->parameters.size(),
			&del->value.front(), del->value.size(), 0
		);
	return *this;
}

Sql::Statement &Sql::Statement::argBlob(const std::uint8_t *bytes, unsigned num)
{
	return argBlob(std::vector<std::uint8_t>(bytes, bytes+num));
}

Sql::Statement &Sql::Statement::arg(const std::uint64_t &d)
{
	shared->parameters.push_back(new BoundValueDeleterT<std::uint64_t>(d));
	
	sqlite3_bind_int64(
			shared->stmt, shared->parameters.size(),
			d
		);
	return *this;
}

Sql::Statement &Sql::Statement::arg(const std::int64_t &d)
{
	shared->parameters.push_back(new BoundValueDeleterT<std::int64_t>(d));
	
	sqlite3_bind_int64(
			shared->stmt, shared->parameters.size(),
			d
		);
	return *this;
}

Sql::Statement &Sql::Statement::arg(const double &d)
{
	shared->parameters.push_back(new BoundValueDeleterT<double>(d));
	
	sqlite3_bind_double(
			shared->stmt, shared->parameters.size(),
			d
		);
	return *this;
}

Sql::Statement &Sql::Statement::argZero(int numBytes)
{
	shared->parameters.push_back(0);

	sqlite3_bind_zeroblob(
			shared->stmt, shared->parameters.size(),
			numBytes
		);
	return *this;
}
Sql::Statement &Sql::Statement::argNull()
{
	shared->parameters.push_back(0);

	sqlite3_bind_null(
			shared->stmt, shared->parameters.size()
		);
	return *this;
}

void Sql::Statement::clearParameters()
{
	sqlite3_reset(shared->stmt);
	for (
			std::vector<BoundValueDeleter*>::iterator i = shared->parameters.begin();
			i !=shared->parameters.end(); ++i
		)
	{
		delete *i;
	}
	shared->parameters.clear();
}

std::string Sql::Statement::error()
{
	std::string e = "SQLite error: "  + std::string(sqlite3_errmsg(shared->db->db))
		+ " <<<" + shared->statement + ">>>(";
	e += boundValues();
	e += ")";
	return e;
}

std::string Sql::Statement::boundValues()
{
	std::string e;
	for (
			std::vector<BoundValueDeleter*>::iterator i = shared->parameters.begin();
			i !=shared->parameters.end(); ++i
		)
	{
		if (i != shared->parameters.begin())
			e += ", ";
		if (*i)
			e += " " + (*i)->asString();
		else
			e += " ?";
	}
	return e;
}


Sql::Statement Sql::statement(const std::string &sql)
{
	Sql::Statement s;
	
	sqlite3_stmt *stmt;
	if (SQLITE_OK != sqlite3_prepare_v2(db, sql.c_str(), sql.length(), &stmt, 0))
		throw std::runtime_error("SQLite error (prepare): "  + std::string(sqlite3_errmsg(db)) + " <<<" + sql + ">>>");
	
	s.shared = new Statement::StatementPrivate;
	s.shared->refs = 1;
	s.shared->stmt = stmt;
	s.shared->db = this;
	s.shared->statement = sql;
	return s;
}

bool Sql::hasTable(const std::string &name)
{
	return 0 < statement("select count(*) from sqlite_master where tbl_name=?").arg(name).execValue<int>();
}


std::string Sql::escape(const std::string &s)
{
	if (s.find('\0') != std::string::npos)
		std::cerr << "Warning: null byte found in sql-escaped string" << std::endl;
	char *e = sqlite3_mprintf("%q", s.data());
	std::string x = e;
	sqlite3_free(e);
	return x;
}

