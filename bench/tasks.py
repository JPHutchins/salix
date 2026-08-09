from camas import Config, Task

run = Task("uv run --package salix-bench python bench.py")

_ = Config(default_task=run)
