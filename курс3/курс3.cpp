#include <iostream>
#include <fstream>   		  // оставлен только для начальной миграции (если нужно)
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <conio.h>
#include <windows.h>
#include <sqlite3.h>  		  // <--- SQLite

using namespace std;

// ============================================================
//  КОНСТАНТЫ
// ============================================================

const string APP_TITLE = "КЛАВИАТУРНЫЙ ТРЕНАЖЕР";
const string DB_FILE = "trainer.db";   // единый файл БД

const int EASY_TIME_LIMIT = 120;
const int MEDIUM_TIME_LIMIT = 90;
const int HARD_TIME_LIMIT = 60;

// Цвета WinAPI
const int COL_DEFAULT = 7;
const int COL_BRIGHT = 15;
const int COL_GREEN = 10;
const int COL_RED = 12;
const int COL_YELLOW = 14;
const int COL_CYAN = 11;
const int COL_DARK_GREY = 8;
const int COL_BG_CURRENT = 112;   // инверсия для текущего символа

// ============================================================
//  СТРУКТУРЫ (без изменений)
// ============================================================

/*
  Схема БД создаётся автоматически при старте (см. функцию DB::init())
  Таблицы:
    users (id INTEGER PRIMARY KEY AUTOINCREMENT,
           username TEXT NOT NULL UNIQUE,
           password TEXT NOT NULL,
           role    TEXT NOT NULL DEFAULT 'user')

    sessions (id INTEGER PRIMARY KEY AUTOINCREMENT,
              username   TEXT NOT NULL,
              difficulty TEXT NOT NULL,
              speed      REAL NOT NULL,
              accuracy   REAL NOT NULL,
              errors     INTEGER NOT NULL,
              duration   INTEGER NOT NULL,
              timestamp  TEXT NOT NULL)

    texts (id INTEGER PRIMARY KEY AUTOINCREMENT,
           difficulty TEXT NOT NULL,
           content    TEXT NOT NULL)
*/

struct User {
    int id;
    string username;
    string password;
    string role;
};

struct SessionRecord {
    int id;
    string username;
    string difficulty;
    double speed;
    double accuracy;
    int errors;
    int duration;
    string timestamp;
};

struct DifficultyConfig {
    string level_name;
    int time_limit;
    string difficulty_key;   // "easy" / "medium" / "hard"
};

struct TrainingSession {
    string target_text;
    string user_input;
    double typing_speed = 0;
    double accuracy = 0;
    int error_count = 0;
};

// ============================================================
//  ПРОСТРАНСТВО ИМЁН DB – теперь работает с SQLite
// ============================================================
namespace DB {

    // Указатель на БД (открывается в main)
    sqlite3* db = nullptr;

    // Проверка ошибок SQLite (аварийное сообщение)
    static void check(int rc, const string& context) {
        if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
            cerr << "SQLite error [" << context << "]: " << sqlite3_errmsg(db) << endl;
            exit(EXIT_FAILURE);
        }
    }

    // Выполнить простой SQL без возврата rows (используется для CREATE, INSERT, DELETE)
    static void exec(const string& sql, const string& context) {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            cerr << "SQLite error [" << context << "]: " << errMsg << endl;
            sqlite3_free(errMsg);
            exit(EXIT_FAILURE);
        }
    }

    // Инициализация БД и создание таблиц
    void init() {
        int rc = sqlite3_open(DB_FILE.c_str(), &db);
        if (rc) {
            cerr << "Can't open database: " << sqlite3_errmsg(db) << endl;
            exit(EXIT_FAILURE);
        }


        // Включаем поддержку внешних ключей (если понадобится)
        exec("PRAGMA foreign_keys = ON;", "pragma");

        // Создаём таблицы, если их нет
        exec(
            "CREATE TABLE IF NOT EXISTS users ("
            "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  username TEXT    NOT NULL UNIQUE,"
            "  password TEXT    NOT NULL,"
            "  role     TEXT    NOT NULL DEFAULT 'user'"
            ");",
            "create users"
        );
        exec(
            "CREATE TABLE IF NOT EXISTS sessions ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  username   TEXT    NOT NULL,"
            "  difficulty TEXT    NOT NULL,"
            "  speed      REAL    NOT NULL,"
            "  accuracy   REAL    NOT NULL,"
            "  errors     INTEGER NOT NULL,"
            "  duration   INTEGER NOT NULL,"
            "  timestamp  TEXT    NOT NULL"
            ");",
            "create sessions"
        );
        exec(
            "CREATE TABLE IF NOT EXISTS texts ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  difficulty TEXT NOT NULL,"
            "  content    TEXT NOT NULL"
            ");",
            "create texts"
        );

        // Если таблица users пуста – добавляем администратора по умолчанию
        sqlite3_stmt* stmt;
        rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users", -1, &stmt, nullptr);
        check(rc, "count users");
        rc = sqlite3_step(stmt);
        int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
        sqlite3_finalize(stmt);
        if (count == 0) {
            exec("INSERT INTO users (username, password, role) VALUES ('admin','admin','admin')",
                "insert default admin");
        }
    }

    // Закрытие БД
    void close() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }


    // --------------------------------------------------------
    //  ПОЛЬЗОВАТЕЛИ
    // --------------------------------------------------------

    vector<User> loadUsers() {
        vector<User> users;
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "SELECT id, username, password, role FROM users ORDER BY id", -1, &stmt, nullptr);
        check(rc, "loadUsers prepare");
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            User u;
            u.id = sqlite3_column_int(stmt, 0);
            u.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            u.password = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            u.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            users.push_back(u);
        }
        sqlite3_finalize(stmt);
        return users;
    }

    User* findUser(vector<User>& users, const string& username) {
        for (auto& u : users)
            if (u.username == username) return &u;
        return nullptr;
    }

    bool registerUser(const string& username, const string& password) {
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "INSERT INTO users (username, password, role) VALUES (?, ?, 'user')", -1, &stmt, nullptr);
        check(rc, "registerUser prepare");
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_CONSTRAINT) {
            sqlite3_finalize(stmt);
            return false;   // пользователь уже существует
        }
        check(rc, "registerUser step");
        sqlite3_finalize(stmt);
        return true;
    }

    void deleteUser(int id) {
        // Не удаляем администратора
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "DELETE FROM users WHERE id = ? AND role != 'admin'", -1, &stmt, nullptr);
        check(rc, "deleteUser prepare");
        sqlite3_bind_int(stmt, 1, id);
        rc = sqlite3_step(stmt);
        check(rc, "deleteUser step");
        sqlite3_finalize(stmt);
    }


    vector<SessionRecord> loadAllSessions() {
        vector<SessionRecord> sessions;
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "SELECT id, username, difficulty, speed, accuracy, errors, duration, timestamp "
            "FROM sessions ORDER BY id", -1, &stmt, nullptr);
        check(rc, "loadAllSessions prepare");
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            SessionRecord r;
            r.id = sqlite3_column_int(stmt, 0);
            r.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            r.difficulty = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            r.speed = sqlite3_column_double(stmt, 3);
            r.accuracy = sqlite3_column_double(stmt, 4);
            r.errors = sqlite3_column_int(stmt, 5);
            r.duration = sqlite3_column_int(stmt, 6);
            r.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            sessions.push_back(r);
        }
        sqlite3_finalize(stmt);
        return sessions;
    }

    void saveSession(const SessionRecord& rec) {
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "INSERT INTO sessions (username, difficulty, speed, accuracy, errors, duration, timestamp) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);
        check(rc, "saveSession prepare");
        sqlite3_bind_text(stmt, 1, rec.username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, rec.difficulty.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, rec.speed);
        sqlite3_bind_double(stmt, 4, rec.accuracy);
        sqlite3_bind_int(stmt, 5, rec.errors);
        sqlite3_bind_int(stmt, 6, rec.duration);
        sqlite3_bind_text(stmt, 7, rec.timestamp.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        check(rc, "saveSession step");
        sqlite3_finalize(stmt);
    }

    vector<SessionRecord> getSessionsByUser(const string& username) {
        vector<SessionRecord> sessions;
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "SELECT id, username, difficulty, speed, accuracy, errors, duration, timestamp "
            "FROM sessions WHERE username = ? ORDER BY id DESC", -1, &stmt, nullptr);
        check(rc, "getSessionsByUser prepare");
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            SessionRecord r;
            r.id = sqlite3_column_int(stmt, 0);
            r.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            r.difficulty = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            r.speed = sqlite3_column_double(stmt, 3);
            r.accuracy = sqlite3_column_double(stmt, 4);
            r.errors = sqlite3_column_int(stmt, 5);
            r.duration = sqlite3_column_int(stmt, 6);
            r.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            sessions.push_back(r);
        }
        sqlite3_finalize(stmt);
        return sessions;
    }

    // --------------------------------------------------------
    //  ТЕКСТЫ (уровни: "easy", "medium", "hard")
    // --------------------------------------------------------
    static void addText(const string& difficulty, const string& content) {
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "INSERT INTO texts (difficulty, content) VALUES (?, ?)", -1, &stmt, nullptr);
        check(rc, "addText prepare");
        sqlite3_bind_text(stmt, 1, difficulty.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, content.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        check(rc, "addText step");
        sqlite3_finalize(stmt);
    }

    static void deleteText(int id) {
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "DELETE FROM texts WHERE id = ?", -1, &stmt, nullptr);
        check(rc, "deleteText prepare");
        sqlite3_bind_int(stmt, 1, id);
        rc = sqlite3_step(stmt);
        check(rc, "deleteText step");
        sqlite3_finalize(stmt);
    }
    // Загрузить все тексты для заданной сложности
    vector<pair<int, string>> loadTextsByDifficulty(const string& difficulty) {
        vector<pair<int, string>> result;   // id + content
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db,
            "SELECT id, content FROM texts WHERE difficulty = ? ORDER BY id", -1, &stmt, nullptr);
        check(rc, "loadTexts prepare");
        sqlite3_bind_text(stmt, 1, difficulty.c_str(), -1, SQLITE_STATIC);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            string content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            result.emplace_back(id, content);
        }
        sqlite3_finalize(stmt);
        if (result.empty()) {
            if (difficulty == "easy") {
                addText("easy", "Программирование на языке Си плюс плюс развивает системное мышление.");
                addText("easy", "Клавиатурный тренажер помогает увеличить скорость набора текста.");
            }
            else if (difficulty == "medium") {
                addText("medium", "Средний уровень сложности. Попробуйте набрать этот текст без ошибок.");
            }
            else if (difficulty == "hard") {
                addText("hard", "Сложный текст для продвинутых пользователей, проверьте свою скорость!");
            }
            return loadTextsByDifficulty(difficulty);   // рекурсивно, чтобы получить с id
        }
        return result;
    }

    

}

void setColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

string getCurrentTimestamp() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    char buf[20];
    localtime_s(&timeinfo, &now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return string(buf);
}

void cls() { system("cls"); }

void printBorder(char ch = '=', int width = 52) {
    setColor(COL_DARK_GREY);
    cout << string(width, ch) << "\n";
    setColor(COL_DEFAULT);
}

void printHeader(const string& title, const string& subtitle = "") {
    printBorder('=');
    setColor(COL_BRIGHT);
    int pad = max(0, (52 - (int)title.size()) / 2);
    cout << string(pad, ' ') << title << "\n";
    setColor(COL_DEFAULT);
    if (!subtitle.empty()) {
        int pad2 = max(0, (52 - (int)subtitle.size()) / 2);
        setColor(COL_CYAN);
        cout << string(pad2, ' ') << subtitle << "\n";
        setColor(COL_DEFAULT);
    }
    printBorder('=');
    cout << "\n";
}

void printProgressBar(int current, int total, int width = 36) {
    int filled = (total > 0) ? (current * width / total) : 0;
    filled = min(filled, width);
    setColor(COL_DARK_GREY);   cout << "[";
    setColor(COL_GREEN);
    for (int i = 0; i < filled; i++) cout << '=';
    if (filled < width) {
        setColor(COL_CYAN);    cout << '>';
        setColor(COL_DARK_GREY);
        for (int i = filled + 1; i < width; i++) cout << ' ';
    }
    setColor(COL_DARK_GREY);   cout << "] ";
    setColor(COL_BRIGHT);
    int pct = total > 0 ? current * 100 / total : 0;
    cout << setw(3) << pct << "%";
    setColor(COL_DEFAULT);
}

void pauseKey(const string& msg = "\nНажмите любую клавишу...") {
    cout << msg;
    _getch();
}


void setDifficultyLevel(DifficultyConfig& config) {
    cout << "\n";
    printBorder('-');
    cout << "  Уровень сложности:\n";
    printBorder('-');
    cout << "  1. Лёгкий  (" << EASY_TIME_LIMIT << " сек)\n";
    cout << "  2. Средний (" << MEDIUM_TIME_LIMIT << " сек)\n";
    cout << "  3. Сложный (" << HARD_TIME_LIMIT << " сек)\n";
    printBorder('-');
    cout << "  Ваш выбор: ";
    int lvl; cin >> lvl;

    if (lvl == 1)      config = { "Лёгкий",  EASY_TIME_LIMIT,   "easy" };
    else if (lvl == 3) config = { "Сложный", HARD_TIME_LIMIT,   "hard" };
    else               config = { "Средний", MEDIUM_TIME_LIMIT, "medium" };

    setColor(COL_GREEN);
    cout << "\n  Уровень \"" << config.level_name << "\" установлен.\n";
    setColor(COL_DEFAULT);
}

string chooseText(const string& difficulty) {
    auto textPairs = DB::loadTextsByDifficulty(difficulty);
    if (textPairs.empty()) return "Текст не найден. Добавьте тексты в панели администратора.";

    vector<string> texts;
    for (const auto& p : textPairs) texts.push_back(p.second);

    cout << "\n";
    printBorder('-');
    cout << "  Выберите текст:\n";
    printBorder('-');
    cout << "  0. Случайный\n";
    for (int i = 0; i < (int)texts.size(); i++) {
        string preview = texts[i].substr(0, min((int)texts[i].size(), 55));
        cout << "  " << i + 1 << ". " << preview;
        if (texts[i].size() > 55) cout << "...";
        cout << "\n";
    }
    printBorder('-');
    cout << "  Ваш выбор: ";
    int ch; cin >> ch;

    if (ch == 0) {
        srand((unsigned)time(nullptr));
        return texts[rand() % texts.size()];
    }
    if (ch >= 1 && ch <= (int)texts.size()) return texts[ch - 1];
    return texts[0];
}

void renderText(const string& target, const string& input) {
    cls();
    printHeader(APP_TITLE, "--- ТРЕНИРОВКА ---");
    cout << "  Текст:\n  ";
    for (int i = 0; i < (int)target.size(); i++) {
        if (i < (int)input.size()) {
            if (input[i] == target[i]) setColor(COL_GREEN);
            else                       setColor(COL_RED);
        }
        else if (i == (int)input.size()) {
            setColor(COL_BG_CURRENT);
        }
        else {
            setColor(COL_YELLOW);
        }
        cout << target[i];
        setColor(COL_DEFAULT);
    }
    cout << "\n\n  ";
    printProgressBar((int)input.size(), (int)target.size());
    cout << "\n\n";
}

void calculateResults(TrainingSession& s, int elapsed) {
    s.typing_speed = elapsed > 0
        ? ((double)s.user_input.length() / elapsed) * 60.0 : 0.0;
    double errRate = (double)s.error_count / max((int)s.target_text.length(), 1);
    s.accuracy = max(0.0, (1.0 - errRate) * 100.0);
}

struct Grade { string label; int color; };
Grade getGrade(double speed, double accuracy) {
    if (accuracy >= 98 && speed >= 300) return { "S  — Мастер печати",    COL_BRIGHT };
    if (accuracy >= 95 && speed >= 220) return { "A  — Отлично",          COL_GREEN };
    if (accuracy >= 90 && speed >= 150) return { "B  — Хорошо",           COL_CYAN };
    if (accuracy >= 80 && speed >= 80)  return { "C  — Удовлетворительно",COL_YELLOW };
    return { "D  — Требуется практика",  COL_RED };
}

void startTraining(const User& currentUser) {
    DifficultyConfig config;
    setDifficultyLevel(config);

    string chosen = chooseText(config.difficulty_key);

    TrainingSession session;
    session.target_text = chosen;

    cout << "\n  Нажмите любую клавишу, чтобы начать...";
    _getch();

    auto startTime = chrono::steady_clock::now();

    while ((int)session.user_input.size() < (int)session.target_text.size()) {
        renderText(session.target_text, session.user_input);

        auto now = chrono::steady_clock::now();
        int elapsed = (int)chrono::duration_cast<chrono::seconds>(now - startTime).count();
        int remaining = config.time_limit - elapsed;
        double liveSpeed = elapsed > 0
            ? ((double)session.user_input.size() / elapsed) * 60.0 : 0.0;

        setColor(remaining <= 10 ? COL_RED : COL_DEFAULT);
        cout << "  Осталось: ";
        setColor(remaining <= 10 ? COL_RED : COL_CYAN);
        cout << setw(3) << remaining << " с";
        setColor(COL_DEFAULT);

        cout << "  |  Скорость: ";
        setColor(COL_GREEN);
        cout << setw(5) << fixed << setprecision(0) << liveSpeed << " зн/мин";
        setColor(COL_DEFAULT);

        cout << "  |  Ошибок: ";
        setColor(session.error_count > 0 ? COL_RED : COL_GREEN);
        cout << session.error_count;
        setColor(COL_DEFAULT);

        cout << "\n\n  Ввод: " << session.user_input;

        if (remaining <= 0) {
            cout << "\n\n";
            setColor(COL_RED);
            cout << "  *** ВРЕМЯ ВЫШЛО! ***\n";
            setColor(COL_DEFAULT);
            pauseKey();
            return;
        }

        unsigned char ch = (unsigned char)_getch();
        if (ch == 8) {   // Backspace
            if (!session.user_input.empty())
                session.user_input.pop_back();
        }
        else if (ch == 27) {   // Escape
            return;
        }
        else if (ch >= 32) {
            size_t pos = session.user_input.size();
            if (pos < session.target_text.size() &&
                ch != (unsigned char)session.target_text[pos])
                session.error_count++;
            session.user_input += (char)ch;
        }
    }

    // Финиш
    auto endTime = chrono::steady_clock::now();
    int elapsed = (int)chrono::duration_cast<chrono::seconds>(endTime - startTime).count();
    calculateResults(session, elapsed);

    SessionRecord rec;
    rec.username = currentUser.username;
    rec.difficulty = config.level_name;
    rec.speed = session.typing_speed;
    rec.accuracy = session.accuracy;
    rec.errors = session.error_count;
    rec.duration = elapsed;
    rec.timestamp = getCurrentTimestamp();
    DB::saveSession(rec);

    cls();
    printHeader(APP_TITLE, "--- РЕЗУЛЬТАТЫ ---");
    Grade g = getGrade(session.typing_speed, session.accuracy);
    printBorder('-');
    cout << "  Пользователь : " << currentUser.username << "\n";
    cout << "  Уровень      : " << config.level_name << "\n";
    cout << "  Время        : " << elapsed << " сек.\n";
    printBorder('-');
    cout << "  Скорость     : ";
    setColor(COL_CYAN);
    cout << fixed << setprecision(1) << session.typing_speed << " зн/мин\n";
    setColor(COL_DEFAULT);
    cout << "  Точность     : ";
    setColor(session.accuracy >= 90 ? COL_GREEN : COL_YELLOW);
    cout << fixed << setprecision(1) << session.accuracy << " %\n";
    setColor(COL_DEFAULT);
    cout << "  Ошибок       : ";
    setColor(session.error_count > 0 ? COL_RED : COL_GREEN);
    cout << session.error_count << "\n";
    setColor(COL_DEFAULT);
    cout << "  Оценка       : ";
    setColor(g.color);
    cout << g.label << "\n";
    setColor(COL_DEFAULT);
    printBorder('-');
    pauseKey();
}


void showUserStatistics(const string& username) {
    cls();
    printHeader(APP_TITLE, "--- МОЯ СТАТИСТИКА ---");
    auto sessions = DB::getSessionsByUser(username);
    if (sessions.empty()) {
        cout << "  Нет записанных тренировок.\n";
        pauseKey(); return;
    }
    double bestSpeed = 0, bestAcc = 0, avgSpeed = 0, avgAcc = 0;
    int totalErrors = 0;
    for (const auto& s : sessions) {
        if (s.speed > bestSpeed)   bestSpeed = s.speed;
        if (s.accuracy > bestAcc)  bestAcc = s.accuracy;
        avgSpeed += s.speed;
        avgAcc += s.accuracy;
        totalErrors += s.errors;
    }
    avgSpeed /= sessions.size();
    avgAcc /= sessions.size();

    setColor(COL_CYAN);
    cout << "  Пользователь    : " << username << "\n";
    setColor(COL_DEFAULT);
    cout << "  Всего тренировок: " << sessions.size() << "\n\n";
    cout << "  Лучшая скорость : ";
    setColor(COL_BRIGHT); cout << fixed << setprecision(1) << bestSpeed << " зн/мин\n"; setColor(COL_DEFAULT);
    cout << "  Средняя скорость: " << fixed << setprecision(1) << avgSpeed << " зн/мин\n";
    cout << "  Лучшая точность : ";
    setColor(COL_BRIGHT); cout << fixed << setprecision(1) << bestAcc << " %\n"; setColor(COL_DEFAULT);
    cout << "  Средняя точность: " << fixed << setprecision(1) << avgAcc << " %\n";
    cout << "  Всего ошибок    : " << totalErrors << "\n\n";

    const int SHOW = 7;
    int start = max(0, (int)sessions.size() - SHOW);
    printBorder('-', 72);
    cout << left
        << setw(4) << " №"
        << setw(10) << " Уровень"
        << setw(12) << " Скорость"
        << setw(12) << " Точность"
        << setw(9) << " Ошибки"
        << " Дата / Время\n";
    printBorder('-', 72);
    for (int i = start; i < (int)sessions.size(); i++) {
        const auto& s = sessions[i];
        cout << " " << setw(3) << (i + 1)
            << " " << setw(9) << s.difficulty
            << " " << setw(8) << fixed << setprecision(1) << s.speed << " з/м"
            << " " << setw(8) << fixed << setprecision(1) << s.accuracy << " %"
            << " " << setw(8) << s.errors
            << " " << s.timestamp << "\n";
    }
    printBorder('-', 72);
    pauseKey();
}


void manageTexts() {
    cls();
    printHeader(APP_TITLE, "--- УПРАВЛЕНИЕ ТЕКСТАМИ ---");
    cout << "  1. easy   (Лёгкий)\n";
    cout << "  2. medium (Средний)\n";
    cout << "  3. hard   (Сложный)\n";
    cout << "\n  Выберите сложность: ";
    int fc; cin >> fc;

    string difficulty;
    if (fc == 1)      difficulty = "easy";
    else if (fc == 3) difficulty = "hard";
    else              difficulty = "medium";

    while (true) {
        cls();
        printHeader(APP_TITLE, "Редактор: " + difficulty);

        auto textPairs = DB::loadTextsByDifficulty(difficulty);
        cout << "  Текстов: " << textPairs.size() << "\n\n";
        for (size_t i = 0; i < textPairs.size(); i++) {
            string preview = textPairs[i].second.substr(0, min((int)textPairs[i].second.size(), 62));
            cout << "  " << setw(3) << (i + 1) << ". [" << textPairs[i].first << "] " << preview;
            if (textPairs[i].second.size() > 62) cout << "...";
            cout << "\n";
        }
        cout << "\n";
        printBorder('-');
        cout << "  1. Добавить текст\n";
        cout << "  2. Удалить текст (по номеру из списка)\n";
        cout << "  0. Назад\n";
        printBorder('-');
        cout << "  Действие: ";
        int action; cin >> action;

        if (action == 0) break;

        if (action == 1) {
            cin.ignore();
            cout << "  Введите новый текст (одна строка):\n  > ";
            string newText; getline(cin, newText);
            if (!newText.empty()) {
                DB::addText(difficulty, newText);
                setColor(COL_GREEN); cout << "\n  Текст добавлен.\n"; setColor(COL_DEFAULT);
            }
            pauseKey();
        }
        else if (action == 2) {
            cout << "  Номер для удаления: ";
            int idx; cin >> idx;
            if (idx >= 1 && idx <= (int)textPairs.size()) {
                int id = textPairs[idx - 1].first;   // получаем реальный ID
                DB::deleteText(id);
                setColor(COL_GREEN); cout << "\n  Удалено.\n"; setColor(COL_DEFAULT);
            }
            else {
                setColor(COL_RED); cout << "\n  Неверный номер.\n"; setColor(COL_DEFAULT);
            }
            pauseKey();
        }
    }
}


void adminViewAllStats() {
    cls();
    printHeader(APP_TITLE, "--- СТАТИСТИКА ВСЕХ ПОЛЬЗОВАТЕЛЕЙ ---");

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(DB::db,
        "SELECT u.username, COUNT(s.id), MAX(s.speed), AVG(s.speed), AVG(s.accuracy) "
        "FROM users u LEFT JOIN sessions s ON u.username = s.username "
        "WHERE u.role = 'user' "
        "GROUP BY u.username "
        "ORDER BY u.username", -1, &stmt, nullptr);
    DB::check(rc, "adminViewAllStats prepare");

    bool hasData = false;
    printBorder('-', 66);
    cout << left
        << setw(18) << "  Пользователь"
        << setw(12) << "Трен."
        << setw(14) << "Лучш. (з/м)"
        << setw(14) << "Средн. (з/м)"
        << "Ср. точн.\n";
    printBorder('-', 66);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        hasData = true;
        string uname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        int cnt = sqlite3_column_int(stmt, 1);
        double best = sqlite3_column_double(stmt, 2);
        double avgSp = sqlite3_column_double(stmt, 3);
        double avgAc = sqlite3_column_double(stmt, 4);

        if (cnt == 0) continue;   // пропускаем пользователей без тренировок

        setColor(COL_CYAN);
        cout << "  " << setw(16) << uname;
        setColor(COL_DEFAULT);
        cout << setw(12) << cnt
            << setw(14) << fixed << setprecision(1) << best
            << setw(14) << fixed << setprecision(1) << avgSp
            << fixed << setprecision(1) << avgAc << " %\n";
    }
    sqlite3_finalize(stmt);

    if (!hasData) {
        cout << "  Нет записей тренировок.\n";
    }
    printBorder('-', 66);
    pauseKey();
}

void adminManageUsers() {
    while (true) {
        cls();
        printHeader(APP_TITLE, "--- УПРАВЛЕНИЕ ПОЛЬЗОВАТЕЛЯМИ ---");

        auto users = DB::loadUsers();
        printBorder('-');
        for (const auto& u : users) {
            cout << "  [" << setw(3) << u.id << "]  ";
            setColor(u.role == "admin" ? COL_YELLOW : COL_DEFAULT);
            cout << setw(18) << u.username;
            setColor(COL_DARK_GREY);
            cout << " (" << u.role << ")\n";
            setColor(COL_DEFAULT);
        }
        printBorder('-');
        cout << "  1. Удалить пользователя\n";
        cout << "  0. Назад\n";
        printBorder('-');
        cout << "  Действие: ";
        int act; cin >> act;
        if (act == 0) break;

        if (act == 1) {
            cout << "  ID пользователя для удаления: ";
            int delId; cin >> delId;
            DB::deleteUser(delId);
            setColor(COL_GREEN);
            cout << "  Готово.\n";
            setColor(COL_DEFAULT);
            pauseKey();
        }
    }
}

void adminMenu() {
    int choice;
    do {
        cls();
        printHeader(APP_TITLE, "--- ПАНЕЛЬ АДМИНИСТРАТОРА ---");
        cout << "  1. Управление текстами\n";
        cout << "  2. Статистика всех пользователей\n";
        cout << "  3. Управление пользователями\n";
        cout << "  0. Выйти из системы\n\n";
        printBorder('-');
        cout << "  Выберите: ";
        cin >> choice;

        switch (choice) {
        case 1: manageTexts();        break;
        case 2: adminViewAllStats();  break;
        case 3: adminManageUsers();   break;
        case 0: break;
        default:
            setColor(COL_RED);
            cout << "  Неверный ввод.\n";
            setColor(COL_DEFAULT);
            pauseKey();
        }
    } while (choice != 0);
}


void userMenu(const User& user) {
    int choice;
    do {
        cls();
        printHeader(APP_TITLE);
        setColor(COL_CYAN);
        cout << "  Добро пожаловать, " << user.username << "!\n\n";
        setColor(COL_DEFAULT);
        cout << "  1. Начать тренировку\n";
        cout << "  2. Моя статистика\n";
        cout << "  0. Выйти из системы\n\n";
        printBorder('-');
        cout << "  Выберите: ";
        cin >> choice;

        switch (choice) {
        case 1: startTraining(user);                break;
        case 2: showUserStatistics(user.username); break;
        case 0: break;
        default:
            setColor(COL_RED);
            cout << "  Неверный ввод.\n";
            setColor(COL_DEFAULT);
            pauseKey();
        }
    } while (choice != 0);
}

void authScreen() {
    while (true) {
        cls();
        printHeader(APP_TITLE, "--- ВХОД / РЕГИСТРАЦИЯ ---");
        cout << "  1. Войти\n";
        cout << "  2. Зарегистрироваться\n";
        cout << "  0. Выход из программы\n\n";
        printBorder('-');
        cout << "  Выберите: ";
        int choice; cin >> choice;

        if (choice == 0) {
            setColor(COL_DEFAULT);
            cout << "\n  До свидания!\n";
            exit(0);
        }

        string username, password;
        cout << "\n  Имя пользователя: ";  cin >> username;
        cout << "  Пароль           : ";
        cin >> password;

        if (choice == 2) {
            if (DB::registerUser(username, password)) {
                setColor(COL_GREEN);
                cout << "\n  Регистрация прошла успешно! Войдите с новыми данными.\n";
            }
            else {
                setColor(COL_RED);
                cout << "\n  Пользователь \"" << username << "\" уже существует.\n";
            }
            setColor(COL_DEFAULT);
            pauseKey();
            continue;
        }

        // Вход
        auto users = DB::loadUsers();
        User* u = DB::findUser(users, username);

        if (u && u->password == password) {
            if (u->role == "admin") adminMenu();
            else                    userMenu(*u);
        }
        else {
            setColor(COL_RED);
            cout << "\n  Неверное имя пользователя или пароль.\n";
            setColor(COL_DEFAULT);
            pauseKey();
        }
    }
}


int main() {
    setlocale(LC_ALL, "Russian");
    SetConsoleCP(1251);
    SetConsoleOutputCP(1251);

    DB::init();  

    authScreen();

    DB::close();
    return 0;
}