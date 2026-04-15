'use strict';

module.exports = {
    env: {
        node: true,
        es2020: true,
    },
    parserOptions: {
        ecmaVersion: 2020,
    },
    rules: {
        'no-unused-vars': ['error', { argsIgnorePattern: '^_' }],
        'no-undef': 'error',
        'no-console': 'off',
        'semi': ['error', 'always'],
        'no-var': 'error',
        'eqeqeq': ['error', 'always'],
    },
};
