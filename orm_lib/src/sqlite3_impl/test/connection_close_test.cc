#include <drogon/drogon_test.h>
#include <drogon/orm/DbClient.h>
#include <sqlite3.h>

#include <iostream>

DROGON_TEST(SQLite3ConnectionCloseTest)
{
    // Initialize SQLite through Drogon before taking the baseline. Run this
    // in a separate executable so other database tests cannot affect it.
    {
        auto client =
            drogon::orm::DbClient::newSqlite3Client("filename=:memory:", 1);
        client->execSqlSync("SELECT 1");
    }
    const auto baseline = sqlite3_memory_used();

    for (bool explicitClose : {true, false})
    {
        for (int iteration = 0; iteration < 3; ++iteration)
        {
            auto client =
                drogon::orm::DbClient::newSqlite3Client("filename=:memory:", 1);
            client->execSqlSync("CREATE TABLE values_test(value INTEGER)");
            // Parameterized queries populate the prepared-statement cache.
            for (int value = 0; value < 3; ++value)
            {
                client->execSqlSync("INSERT INTO values_test VALUES(?)", value);
                const auto rows = client->execSqlSync(
                    "SELECT value FROM values_test WHERE value = ?", value);
                REQUIRE(rows.size() == 1);
                CHECK(rows[0][0].as<int>() == value);
            }
            REQUIRE(sqlite3_memory_used() > baseline);
            if (explicitClose)
            {
                client->closeAll();
                client->closeAll();
            }
            client.reset();
            CHECK(sqlite3_memory_used() == baseline);
        }
    }
}

int main(int argc, char **argv)
{
    // Enable accounting even when SQLite was built with MEMSTATUS disabled
    // by default. Drogon still chooses its threading mode before first use.
    if (sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 1) != SQLITE_OK)
    {
        std::cerr << "Could not enable SQLite memory accounting\n";
        return 1;
    }
    return drogon::test::run(argc, argv);
}
