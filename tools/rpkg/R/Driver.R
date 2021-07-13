DBDIR_MEMORY <- ":memory:"

check_flag <- function(x) {
  if (is.null(x) || length(x) != 1 || is.na(x) || !is.logical(x)) {
    stop("flags need to be scalar logicals")
  }
}

extptr_str <- function(e, n = 5) {
  x <- .Call(duckdb_ptr_to_str, e)
  substr(x, nchar(x) - n + 1, nchar(x))
}

drv_to_string <- function(drv) {
  if (!is(drv, "duckdb_driver")) {
    stop("pass a duckdb_driver object")
  }
  sprintf("<duckdb_driver %s dbdir='%s' read_only=%s>", extptr_str(drv@database_ref), drv@dbdir, drv@read_only)
}

#' @rdname duckdb_driver-class
#' @inheritParams methods::show
#' @export
setMethod(
  "show", "duckdb_driver",
  function(object) {
    message(drv_to_string(object))
    invisible(NULL)
  }
)

#' Connect to a DuckDB database instance
#'
#' `dbConnect()` connects to a database instance.
#'
#' @param drv Object returned by `duckdb()`
#' @param dbdir Location for database files. Should be a path to an existing
#'   directory in the file system. With the default, all
#'   data is kept in RAM
#' @param ... Ignored
#' @param debug Print additional debug information such as queries
#' @param read_only Set to `TRUE` for read-only operation
#' @param timezone_out The time zone returned to R, defaults to `"UTC"`, which
#'   is currently the only timezone supported by duckdb.
#'   If you want to display datetime values in the local timezone,
#'   set to [Sys.timezone()] or `""`.
#' @param tz_out_convert How to convert timestamp columns to the timezone specified
#'   in `timezone_out`. There are two options: `"with"`, and `"force"`. If `"with"`
#'   is chosen, the timestamp will be returned as it would appear in the specified time zone.
#'   If `"force"` is chosen, the timestamp will have the same clock
#'   time as the timestamp in the database, but with the new time zone.
#' @param config Named list with DuckDB configuration flags
#'
#' @return `dbConnect()` returns an object of class
#'   \linkS4class{duckdb_connection}.
#'
#' @rdname duckdb
#' @export
#' @examples
#' drv <- duckdb()
#' con <- dbConnect(drv)
#'
#' dbGetQuery(con, "SELECT 'Hello, world!'")
#'
#' dbDisconnect(con)
#' duckdb_shutdown(drv)
#'
#' # Shorter:
#' con <- dbConnect(duckdb())
#' dbGetQuery(con, "SELECT 'Hello, world!'")
#' dbDisconnect(con, shutdown = TRUE)
setMethod(
  "dbConnect", "duckdb_driver",
  function(drv, dbdir = DBDIR_MEMORY, ...,
           debug = getOption("duckdb.debug", FALSE),
           read_only = FALSE,
           timezone_out = "UTC",
           tz_out_convert = c("with", "force"), config = list()) {

    check_flag(debug)
    timezone_out <- check_tz(timezone_out)
    tz_out_convert <- match.arg(tz_out_convert)

    missing_dbdir <- missing(dbdir)
    dbdir <- path.expand(as.character(dbdir))

    # aha, a late comer. let's make a new instance.
    if (!missing_dbdir && dbdir != drv@dbdir) {
      duckdb_shutdown(drv)
      drv <- duckdb(dbdir, read_only, config)
    }

    conn <- duckdb_connection(drv, debug = debug)
    on.exit(dbDisconnect(conn))

    conn@timezone_out <- timezone_out
    conn@tz_out_convert <- tz_out_convert

    on.exit(NULL)

    # perform the connection notification at the top level, to ensure that it's had
    # a chance to get its external pointer connected, and so we can capture the
    # expression that created it
    # this only has effect in RStudio and interactive sessions
    if (!is.null(getOption("connectionObserver")) && !isTRUE(debug) && interactive()) { # nocov start
      addTaskCallback(function(expr, ...) {
        tryCatch({
          if (is.call(expr) &&
              as.character(expr[[1]]) %in% c("<-", "=") &&
              "dbConnect" %in% as.character(expr[[3]][[1]])) {

            # notify if this is an assignment we can replay
            on_connection_opened(eval(expr[[2]]))
          }
        }, error = function(e) {
          warning("Could not notify connection observer. ", e$message, call. = FALSE)
        })

        # always return false so the task callback is run at most once
        FALSE
      })
    } # nocov end

    conn
  }
)

#' @description
#' `dbDisconnect()` closes a DuckDB database connection, optionally shutting down
#' the associated instance.
#'
#' @param conn A `duckdb_connection` object
#' @param shutdown Set to `TRUE` to shut down the DuckDB database instance that this connection refers to.
#' @rdname duckdb
#' @export
setMethod(
  "dbDisconnect", "duckdb_connection",
  function(conn, ..., shutdown = FALSE) {
    if (!dbIsValid(conn)) {
      warning("Connection already closed.", call. = FALSE)
    }
    .Call(duckdb_disconnect_R, conn@conn_ref)
    if (shutdown) {
      duckdb_shutdown(conn@driver)
    }
    on_connection_closed(conn)
    invisible(TRUE)
  }
)

#' @description
#' `duckdb()` creates or reuses a database instance.
#'
#' @return `duckdb()` returns an object of class \linkS4class{duckdb_driver}.
#'
#' @import methods DBI
#' @export
duckdb <- function(dbdir = DBDIR_MEMORY, read_only = FALSE, config=list()) {
  check_flag(read_only)
  new(
    "duckdb_driver",
    database_ref = .Call(duckdb_startup_R, dbdir, read_only, config),
    dbdir = dbdir,
    read_only = read_only
  )
}

#' @rdname duckdb_driver-class
#' @export
setMethod(
  "dbDataType", "duckdb_driver",
  function(dbObj, obj, ...) {

    if (is.null(obj)) stop("NULL parameter")
    if (is.data.frame(obj)) {
      return(vapply(obj, function(x) dbDataType(dbObj, x), FUN.VALUE = "character"))
    }
    #  else if (int64 && inherits(obj, "integer64")) "BIGINT"
    else if (inherits(obj, "Date")) {
      "DATE"
    } else if (inherits(obj, "difftime")) {
      "TIME"
    } else if (is.logical(obj)) {
      "BOOLEAN"
    } else if (is.integer(obj)) {
      "INTEGER"
    } else if (is.numeric(obj)) {
      "DOUBLE"
    } else if (inherits(obj, "POSIXt")) {
      "TIMESTAMP"
    } else if (is.list(obj) && all(vapply(obj, typeof, FUN.VALUE = "character") == "raw" || is.na(obj))) {
      "BLOB"
    } else {
      "STRING"
    }

  }
)

#' @rdname duckdb_driver-class
#' @inheritParams DBI::dbIsValid
#' @importFrom DBI dbConnect
#' @export
setMethod(
  "dbIsValid", "duckdb_driver",
  function(dbObj, ...) {
    valid <- FALSE
    tryCatch(
      {
        con <- dbConnect(dbObj)
        dbExecute(con, SQL("SELECT 1"))
        dbDisconnect(con)
        valid <- TRUE
      },
      error = function(c) {
      }
    )
    valid
  }
)

#' @rdname duckdb_driver-class
#' @inheritParams DBI::dbGetInfo
#' @export
setMethod(
  "dbGetInfo", "duckdb_driver",
  function(dbObj, ...) {
    con <- dbConnect(dbObj)
    version <- dbGetQuery(con, "select library_version from pragma_version()")[[1]][[1]]
    dbDisconnect(con)
    list(driver.version = version, client.version = version, dbname=dbObj@dbdir)
  }
)


#' @description
#' `duckdb_shutdown()` shuts down a database instance.
#'
#' @return `dbDisconnect()` and `duckdb_shutdown()` are called for their
#'   side effect.
#' @rdname duckdb
#' @export
duckdb_shutdown <- function(drv) {
  if (!is(drv, "duckdb_driver")) {
    stop("pass a duckdb_driver object")
  }
  if (!dbIsValid(drv)) {
    warning("invalid driver object, already closed?")
    invisible(FALSE)
  }
  .Call(duckdb_shutdown_R, drv@database_ref)
  invisible(TRUE)
}

is_installed <- function(pkg) {
  as.logical(requireNamespace(pkg, quietly = TRUE)) == TRUE
}

check_tz <- function(timezone) {

  if (!is.null(timezone) && timezone == "") {
    return(Sys.timezone())
  }

  if (is.null(timezone) || !timezone %in% OlsonNames()) {
    warning(
      "Invalid time zone '", timezone, "', ",
      "falling back to UTC.\n",
      "Set the `timezone_out` argument to a valid time zone.\n",
      call. = FALSE
    )
    return("UTC")
  }

  timezone
}
