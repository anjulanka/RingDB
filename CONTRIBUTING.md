# 🤝 Contributing to RingDB

Thank you for your interest in making RingDB faster! To maintain the extreme performance and safety of our pure C engine, all contributors must follow these strict guidelines:

## 🧭 The Rule Book

1. **Open an Issue First**: Before writing any code, you must open a GitHub Issue explaining the bug you want to fix or the optimization you want to add. Wait for a maintainer to approve the concept.
2. **Strict Code Quality**: We write in modern, clean, pure C. No C++ style syntax, no unsafe functions (like `strcpy`), and absolutely zero memory leaks are allowed. 
3. **Run the Benchmarks**: If you are optimizing a core component (like `io_uring_backend.c` or `ring_highway.c`), you must provide benchmark logs proving your change does not drop our overall QPS.

## 🔏 Legal Contributor Agreement (CLA)
By contributing code to RingDB, you grant "The RingDB Authors" a permanent, worldwide, royalty-free license to use, modify, and distribute your code under our Tri-License or any future commercial terms. You retain copyright ownership but agree not to restrict our business roadmap.
