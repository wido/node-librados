export class CephError extends Error {
  readonly code: number;
  readonly errno: number;
  /** Symbolic errno name such as "ENOENT", when the C library can name it */
  readonly codeName?: string;
  readonly operation: string;
  readonly resource?: string;

  constructor(message: string, options: {code: number; errno: number; codeName?: string; operation: string; resource?: string; cause?: unknown}) {
    super(message, {cause: options.cause});
    this.name = "CephError";
    this.code = options.code;
    this.errno = options.errno;
    this.codeName = options.codeName;
    this.operation = options.operation;
    this.resource = options.resource;
  }

  static [Symbol.hasInstance](value: unknown): boolean {
    return Boolean(value && typeof value === "object" &&
      "code" in value && typeof value.code === "number" &&
      "operation" in value && typeof value.operation === "string");
  }

  static normalize(error: unknown): CephError {
    if (error instanceof CephError) return error;
    if (error && typeof error === "object" && "code" in error && "operation" in error) {
      const value = error as Error & {code: number; errno?: number; codeName?: string; operation: string; resource?: string};
      return new CephError(value.message, {code: value.code, errno: value.errno ?? Math.abs(value.code), codeName: value.codeName, operation: value.operation, resource: value.resource, cause: error});
    }
    throw error;
  }
}

export class CephNotSupportedError extends Error {
  constructor(readonly capability: string) {
    super(`The installed Ceph client does not support ${capability}`);
    this.name = "CephNotSupportedError";
  }
}

export class CephConfirmationError extends Error {
  constructor(operation: string, expected: string) {
    super(`${operation} requires { confirm: ${JSON.stringify(expected)} }`);
    this.name = "CephConfirmationError";
  }
}
