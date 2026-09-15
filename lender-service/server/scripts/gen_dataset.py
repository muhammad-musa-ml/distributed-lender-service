#!/usr/bin/env python3
"""Generate a synthetic, schema-compatible HMDA-shaped dataset for the p4 MySQL image.

WHY THIS EXISTS. `lender-service/Dockerfile.mysql` copies `hdma-wi-2021.sql.gz` into the MySQL
entrypoint directory, and that file is NOT in this checkout -- it was course-provided and
was never committed. Without it the `mysql` service comes up with an empty CS544 database
and DbToHdfs has nothing to read.

WHY SYNTHETIC RATHER THAN THE REAL FILE. Three reasons, in order of weight:
  1. The real file is not available here at all.
  2. This repository is public. Redistributing a course-provided dataset is a question this
     project does not need to answer; a generator sidesteps it entirely.
  3. A generator makes the fixture reproducible for anyone who clones the repo -- one
     command and a fixed seed, instead of a 5 MB binary blob nobody can regenerate.

WHAT IS AND IS NOT REAL. The SCHEMA, the FIPS county codes, and the HMDA loan-type codes
are real. The ROWS are generated. Any figure this dataset produces -- row counts, average
loan amounts -- describes THIS generated fixture and nothing about actual Wisconsin lending.
The published README says so in the same words.

Deterministic: a fixed seed means two runs produce byte-identical output, so a row count
quoted anywhere can be re-derived rather than taken on trust.

Usage:
    python3 gen_dataset.py [--rows N] [--out PATH] [--seed S]

Default output is lender-service/hdma-wi-2021.sql.gz -- exactly the filename and
Dockerfile.mysql already expects, so that file needs no edit.
"""

import argparse
import gzip
import os
import random
import sys

# Real HMDA loan-type codes and names.
LOAN_TYPES = [
    (1, "Conventional"),
    (2, "FHA-insured"),
    (3, "VA-guaranteed"),
    (4, "USDA Rural Housing Service or Farm Service Agency"),
]

# Real Wisconsin county FIPS codes (55XXX, odd-numbered), with a relative weight standing in
# for population so the row distribution is lumpy the way real data is, rather than uniform.
# Milwaukee (55079) and Dane (55025) dominate, exactly as they do in practice.
COUNTY_WEIGHTS = {
    55079: 380, 55025: 300, 55133: 190, 55087: 110, 55139: 100, 55009: 95,
    55101: 80, 55059: 75, 55131: 62, 55089: 55, 55055: 45, 55105: 44,
    55127: 42, 55071: 38, 55073: 37, 55141: 33, 55045: 27, 55049: 22,
    55035: 32, 55015: 24, 55017: 25, 55109: 26, 55093: 24, 55117: 30,
    55027: 25, 55039: 23, 55111: 22, 55057: 15, 55021: 16, 55043: 15,
    55047: 12, 55053: 11, 55063: 35, 55065: 9, 55069: 10, 55077: 8,
    55081: 14, 55083: 16, 55085: 12, 55091: 4, 55095: 21, 55097: 26,
    55099: 7, 55103: 8, 55107: 6, 55113: 7, 55115: 16, 55119: 8,
    55121: 9, 55123: 12, 55125: 8, 55129: 6, 55135: 17, 55137: 12,
    55001: 8, 55003: 5, 55005: 20, 55007: 5, 55011: 5, 55013: 4,
    55019: 12, 55023: 6, 55029: 12, 55031: 19, 55033: 20, 55037: 4,
    55041: 4, 55051: 4, 55061: 10, 55067: 11, 55075: 18, 55078: 2,
}


def build_rows(n_rows, seed):
    """Yield (county_code, loan_type, loan_amount, income, interest_rate) tuples."""
    rng = random.Random(seed)
    counties = list(COUNTY_WEIGHTS.keys())
    weights = [COUNTY_WEIGHTS[c] for c in counties]
    type_ids = [t[0] for t in LOAN_TYPES]
    # Conventional dominates real HMDA data by a wide margin.
    type_weights = [70, 18, 8, 4]

    for _ in range(n_rows):
        county = rng.choices(counties, weights=weights, k=1)[0]
        ltype = rng.choices(type_ids, weights=type_weights, k=1)[0]

        # Log-normal-ish amounts in whole thousands, which is how HMDA reports them. The
        # spread deliberately puts a real fraction of rows OUTSIDE the 30k..800k window so
        # DbToHdfs's filter actually removes something and can be seen to work.
        amount = int(rng.lognormvariate(12.3, 0.62) / 1000.0) * 1000
        if amount < 1000:
            amount = 1000
        if amount > 5_000_000:
            amount = 5_000_000

        # income is nullable in HMDA and genuinely missing sometimes; keep that, because the
        # Arrow builder path for nulls should be exercised by real data and not only by a
        # unit test.
        if rng.random() < 0.04:
            income = None
        else:
            income = max(8, int(rng.lognormvariate(4.5, 0.55)))

        if rng.random() < 0.06:
            rate = None
        else:
            rate = round(rng.uniform(2.35, 7.85), 3)

        yield (county, ltype, amount, income, rate)


def sql_value(v):
    return "NULL" if v is None else str(v)


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.abspath(os.path.join(here, "..", "..", "hdma-wi-2021.sql.gz"))

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", type=int, default=450_000, help="rows to generate")
    ap.add_argument("--out", default=default_out, help="output .sql.gz path")
    ap.add_argument("--seed", type=int, default=544, help="RNG seed (fixed for reproducibility)")
    args = ap.parse_args(argv)

    batch = []
    total = 0
    in_window = 0
    out_dir = os.path.dirname(args.out)
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    with gzip.open(args.out, "wt", encoding="utf-8", newline="\n") as fh:
        fh.write("-- Synthetic HMDA-shaped fixture for CS544 p4. Generated by\n")
        fh.write("-- server/scripts/gen_dataset.py -- rows are FABRICATED.\n")
        fh.write("-- Schema, FIPS county codes and HMDA loan-type codes are real.\n")
        fh.write("CREATE DATABASE IF NOT EXISTS CS544;\n")
        fh.write("USE CS544;\n\n")
        fh.write("DROP TABLE IF EXISTS loans;\n")
        fh.write("DROP TABLE IF EXISTS loan_types;\n\n")
        fh.write(
            "CREATE TABLE loan_types (\n"
            "  loan_type_id INT NOT NULL PRIMARY KEY,\n"
            "  loan_type_name VARCHAR(64) NOT NULL\n"
            ") ENGINE=InnoDB;\n\n"
        )
        fh.write("INSERT INTO loan_types (loan_type_id, loan_type_name) VALUES\n")
        fh.write(",\n".join("  (%d, '%s')" % (i, name.replace("'", "''"))
                            for i, name in LOAN_TYPES))
        fh.write(";\n\n")
        fh.write(
            "CREATE TABLE loans (\n"
            "  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,\n"
            "  county_code INT NOT NULL,\n"
            "  loan_type INT NOT NULL,\n"
            "  loan_amount INT NOT NULL,\n"
            "  income INT NULL,\n"
            "  interest_rate DOUBLE NULL,\n"
            "  KEY idx_county_code (county_code),\n"
            "  CONSTRAINT fk_loans_type FOREIGN KEY (loan_type)\n"
            "    REFERENCES loan_types (loan_type_id)\n"
            ") ENGINE=InnoDB;\n\n"
        )
        # One multi-row INSERT per 1,000 rows. Row-at-a-time INSERTs would make this file
        # several times larger and take minutes to import instead of seconds.
        for row in build_rows(args.rows, args.seed):
            county, ltype, amount, income, rate = row
            total += 1
            if 30_000 < amount < 800_000:
                in_window += 1
            batch.append("(%d,%d,%d,%s,%s)" % (county, ltype, amount,
                                               sql_value(income), sql_value(rate)))
            if len(batch) >= 1000:
                fh.write("INSERT INTO loans (county_code,loan_type,loan_amount,income,"
                         "interest_rate) VALUES\n")
                fh.write(",\n".join(batch))
                fh.write(";\n")
                batch = []
        if batch:
            fh.write("INSERT INTO loans (county_code,loan_type,loan_amount,income,"
                     "interest_rate) VALUES\n")
            fh.write(",\n".join(batch))
            fh.write(";\n")

    size = os.path.getsize(args.out)
    # Print the population, not just "done". These two numbers are the only honest source for
    # any row count quoted about this fixture.
    print("wrote %s" % args.out)
    print("  rows generated            : %d" % total)
    print("  rows inside 30k..800k     : %d  (%.2f%%)" % (in_window, 100.0 * in_window / total))
    print("  rows DbToHdfs will exclude: %d" % (total - in_window))
    print("  gzipped size              : %d bytes" % size)
    print("  seed                      : %d  (deterministic)" % args.seed)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
