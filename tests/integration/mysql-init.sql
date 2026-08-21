CREATE TABLE IF NOT EXISTS sys_dict_type (
    id INT PRIMARY KEY,
    name VARCHAR(64) NOT NULL
);

INSERT INTO sys_dict_type (id, name) VALUES (1, 'integration')
ON DUPLICATE KEY UPDATE name = VALUES(name);
