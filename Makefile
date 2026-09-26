.PHONY: install lint format test test-golden test-integration migrate run clean

install:
	python -m pip install -r requirements.txt -r requirements-dev.txt

lint:
	python -m ruff check .
	python -m ruff format --check .

format:
	python -m ruff check --fix .
	python -m ruff format .

test:
	python -m pytest

test-golden:
	python -m pytest -m golden

test-integration:
	python -m pytest -m integration

migrate:
	python manage.py migrate

run:
	python manage.py runserver

clean:
	-rm -rf .pytest_cache .ruff_cache cache
	-find . -type d -name __pycache__ -exec rm -rf {} +
