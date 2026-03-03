#pragma once

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace sat_parallel {

struct CNFFormula {
    int num_vars = 0;
    int num_clauses = 0;
    std::vector<std::vector<int>> clauses;
};

inline CNFFormula parse_dimacs(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) throw std::runtime_error("Cannot open CNF file: " + path);

    CNFFormula formula;
    char line[4096];

    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == 'c' || line[0] == '\n' || line[0] == '\r') continue;
        if (line[0] == 'p') {
            char fmt[16];
            std::sscanf(line, "p %15s %d %d", fmt, &formula.num_vars, &formula.num_clauses);
            break;
        }
    }

    formula.clauses.reserve(formula.num_clauses);
    std::vector<int> clause;

    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == 'c' || line[0] == '\n' || line[0] == '\r') continue;

        const char* p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) break;

            int lit = 0;
            int sign = 1;
            if (*p == '-') { sign = -1; p++; }
            if (*p < '0' || *p > '9') { p++; continue; }
            while (*p >= '0' && *p <= '9') {
                lit = lit * 10 + (*p - '0');
                p++;
            }
            lit *= sign;

            if (lit == 0) {
                if (!clause.empty()) {
                    formula.clauses.push_back(std::move(clause));
                    clause.clear();
                }
            } else {
                clause.push_back(lit);
            }
        }
    }

    if (!clause.empty()) {
        formula.clauses.push_back(std::move(clause));
    }

    std::fclose(f);
    return formula;
}

}  // namespace sat_parallel
